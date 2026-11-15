// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/*
 * Raw I/Q over the network (issue #743): the iqtcp:/iqudp: spec parser and its canonical text, every wire format's
 * conversion (rails, little-endian assembly, cf32 bounding pinned to the rule the SoapySDR and cf32 replay paths
 * use), the TCP partial-sample carry at every split, the rate telemetry rows, and the socket reader on loopback: TCP
 * 1-byte writes, carry discarded at a reconnect, stall vs close, backoff and stops that stay prompt, connect and bind
 * refusals; UDP 1472-byte, empty and malformed datagrams, one sender, re-lock after a stall.
 *
 * Real: the module, sockets on 127.0.0.1 with ephemeral ports. Simulated: the senders (threads or this thread) and
 * the clock the reader's stall detection and telemetry read, which a test advances by hand.
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/dsp/simd_widen.h>
#include <dsd-neo/io/iq_net.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/input_failure.h>
#include <dsd-neo/runtime/log.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !DSD_PLATFORM_WIN_NATIVE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

#include "iq_net_internal.h"

#ifdef MSG_NOSIGNAL
#define TEST_SEND_FLAGS MSG_NOSIGNAL
#else
#define TEST_SEND_FLAGS 0
#endif

/* Long enough for a sanitizer build on a loaded runner; a passing run waits far less. */
enum { k_wait_ms = 5000, k_prompt_ms = 1500 };

static int g_failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            DSD_FPRINTF(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #cond);                                       \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

static int
float_bits_equal(float a, float b) {
    uint32_t ua = 0, ub = 0;
    DSD_MEMCPY(&ua, &a, sizeof(ua));
    DSD_MEMCPY(&ub, &b, sizeof(ub));
    return ua == ub;
}

static float
float_from_bits(uint32_t bits) {
    float f = 0.0f;
    DSD_MEMCPY(&f, &bits, sizeof(f));
    return f;
}

static void
put_le32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
    p[2] = (uint8_t)((v >> 16) & 0xFFU);
    p[3] = (uint8_t)(v >> 24);
}

/* ---- Log tap: counts the once-per-run lines ---------------------------------------------------------------- */

static atomic_int g_log_malformed;
static atomic_int g_log_foreign;
static atomic_int g_log_rate;

static void
log_tap(dsd_neo_log_level_t level, const char* text, void* ctx) {
    (void)level;
    (void)ctx;
    if (strstr(text, "set the sender's packet size to a multiple of")) {
        (void)atomic_fetch_add(&g_log_malformed, 1);
    }
    if (strstr(text, "ignoring datagrams from")) {
        (void)atomic_fetch_add(&g_log_foreign, 1);
    }
    if (strstr(text, "samples/s are arriving")) {
        (void)atomic_fetch_add(&g_log_rate, 1);
    }
}

/* ---- Spec ------------------------------------------------------------------------------------------------------ */

typedef struct {
    const char* text;
    dsd_iq_net_kind kind;
    const char* host;
    int port;
    uint32_t rate_hz;
    dsd_iq_net_format format;
    uint32_t freq_hz;
    const char* canonical;
} spec_ok_row;

static void
test_spec_parse_ok(void) {
    static const spec_ok_row rows[] = {
        {"iqtcp:127.0.0.1:1234:48000:cs16", DSD_IQ_NET_KIND_TCP, "127.0.0.1", 1234, 48000U, DSD_IQ_NET_FORMAT_CS16, 0U,
         "iqtcp:127.0.0.1:1234:48000:cs16"},
        {"iqtcp:localhost:1234:240k:cu8:851.0125M", DSD_IQ_NET_KIND_TCP, "localhost", 1234, 240000U,
         DSD_IQ_NET_FORMAT_CU8, 851012500U, "iqtcp:localhost:1234:240000:cu8:851012500"},
        {"iqudp:0.0.0.0:7355:1.152M:CF32:", DSD_IQ_NET_KIND_UDP, "0.0.0.0", 7355, 1152000U, DSD_IQ_NET_FORMAT_CF32, 0U,
         "iqudp:0.0.0.0:7355:1152000:cf32"},
        {"iqudp:192.0.2.5:1:8000:cs8:0", DSD_IQ_NET_KIND_UDP, "192.0.2.5", 1, 8000U, DSD_IQ_NET_FORMAT_CS8, 0U,
         "iqudp:192.0.2.5:1:8000:cs8"},
        {"iqtcp:h:65535:4294967295:Cs32:4294967295", DSD_IQ_NET_KIND_TCP, "h", 65535, 4294967295U,
         DSD_IQ_NET_FORMAT_CS32, 4294967295U, "iqtcp:h:65535:4294967295:cs32:4294967295"},
        {"iqtcp:h:5:8k:cs16:4294.967295M", DSD_IQ_NET_KIND_TCP, "h", 5, 8000U, DSD_IQ_NET_FORMAT_CS16, 4294967295U,
         "iqtcp:h:5:8000:cs16:4294967295"},
        {"iqtcp:h:5:48.5k:cs16:100.5", DSD_IQ_NET_KIND_TCP, "h", 5, 48500U, DSD_IQ_NET_FORMAT_CS16, 101U,
         "iqtcp:h:5:48500:cs16:101"},
        {"iqtcp:h:5:.5M:cs16:100.4999", DSD_IQ_NET_KIND_TCP, "h", 5, 500000U, DSD_IQ_NET_FORMAT_CS16, 100U,
         "iqtcp:h:5:500000:cs16:100"},
        {"iqtcp:h:5:96000.000:cs16:1.0000005k", DSD_IQ_NET_KIND_TCP, "h", 5, 96000U, DSD_IQ_NET_FORMAT_CS16, 1000U,
         "iqtcp:h:5:96000:cs16:1000"},
        {"iqtcp:h:5:1.152M:cs16:0.4G", DSD_IQ_NET_KIND_TCP, "h", 5, 1152000U, DSD_IQ_NET_FORMAT_CS16, 400000000U,
         "iqtcp:h:5:1152000:cs16:400000000"},
        {"iqudp:h:0099:48K:cs16:1m", DSD_IQ_NET_KIND_UDP, "h", 99, 48000U, DSD_IQ_NET_FORMAT_CS16, 1000000U,
         "iqudp:h:99:48000:cs16:1000000"},
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        const spec_ok_row* row = &rows[i];
        dsd_iq_net_spec spec;
        char err[256];
        const int rc = dsd_iq_net_spec_parse(row->text, &spec, err, sizeof(err));
        if (rc != 0) {
            DSD_FPRINTF(stderr, "row %zu '%s': refused: %s\n", i, row->text, err);
            g_failures++;
            continue;
        }
        CHECK(dsd_iq_net_spec_kind(row->text) == row->kind);
        CHECK(spec.kind == row->kind);
        CHECK(strcmp(spec.host, row->host) == 0);
        CHECK(spec.port == row->port);
        CHECK(spec.rate_hz == row->rate_hz);
        CHECK(spec.format == row->format);
        CHECK(spec.freq_hz == row->freq_hz);
        char text[DSD_IQ_NET_SPEC_TEXT_MAX];
        const int len = dsd_iq_net_spec_format(&spec, text, sizeof(text));
        CHECK(len == (int)strlen(row->canonical));
        CHECK(strcmp(text, row->canonical) == 0);
        dsd_iq_net_spec again;
        CHECK(dsd_iq_net_spec_parse(text, &again, NULL, 0) == 0);
        CHECK(memcmp(&again, &spec, sizeof(spec)) == 0);
    }
}

typedef struct {
    const char* text;
    const char* reason; /* a part of the message that names the field */
} spec_bad_row;

static void
test_spec_parse_refusals(void) {
    char long_host[400];
    DSD_MEMSET(long_host, 0, sizeof(long_host));
    DSD_SNPRINTF(long_host, sizeof(long_host), "iqtcp:");
    for (size_t i = 6; i < 6U + 300U; i++) {
        long_host[i] = 'a';
    }
    DSD_SNPRINTF(long_host + 306, sizeof(long_host) - 306, ":1234:48k:cs16");
    const spec_bad_row rows[] = {
        {"tcp:127.0.0.1:1234", "not an iqtcp: or iqudp: spec"},
        {"IQTCP:h:1:48k:cs16", "not an iqtcp: or iqudp: spec"},
        {"iqtcp:", "iqtcp: HOST is empty"},
        {"iqtcp::1234:48k:cs16", "HOST is empty"},
        {"iqudp::1234:48k:cs16", "iqudp: BIND is empty"},
        {long_host, "HOST is longer than 255 characters"},
        {"iqtcp:h", "PORT is missing"},
        {"iqtcp:h::48k:cs16", "PORT is empty"},
        {"iqtcp:h:0:48k:cs16", "PORT '0' is not a port number from 1 to 65535"},
        {"iqudp:h:0:48k:cs16", "PORT '0'"},
        {"iqtcp:h:65536:48k:cs16", "PORT '65536'"},
        {"iqtcp:h:12x:48k:cs16", "PORT '12x'"},
        {"iqtcp:h:+12:48k:cs16", "PORT '+12'"},
        {"iqtcp:h: 12:48k:cs16", "PORT ' 12'"},
        {"iqtcp:h:12", "RATE is missing"},
        {"iqtcp:h:12::cs16", "RATE is empty"},
        {"iqtcp:h:12:7999:cs16", "RATE '7999' is outside 8000 to 4294967295"},
        {"iqtcp:h:12:4294967296:cs16", "RATE '4294967296' is outside"},
        {"iqtcp:h:12:4295M:cs16", "RATE '4295M' is outside"},
        {"iqtcp:h:12:48000.5:cs16", "RATE '48000.5' is not a whole number of samples per second"},
        {"iqtcp:h:12:1.0000005M:cs16", "RATE '1.0000005M' is not a whole number"},
        {"iqtcp:h:12:48kHz:cs16", "RATE '48kHz' is not a sample rate"},
        {"iqtcp:h:12:48G:cs16", "RATE '48G' is not a sample rate"},
        {"iqtcp:h:12:1e5:cs16", "RATE '1e5'"},
        {"iqtcp:h:12:-48k:cs16", "RATE '-48k'"},
        {"iqtcp:h:12:k:cs16", "RATE 'k'"},
        {"iqtcp:h:12:4.8.0k:cs16", "RATE '4.8.0k'"},
        {"iqtcp:h:12:48k", "FORMAT is missing"},
        {"iqtcp:h:12:48k:", "FORMAT is empty"},
        {"iqtcp:h:12:48k:s16", "FORMAT 's16' is not one of cu8, cs8, cs16, cs32, cf32"},
        {"iqtcp:h:12:48k:cs16le", "FORMAT 'cs16le'"},
        {"iqtcp:h:12:48k:cs16:abc", "FREQ 'abc' is not a frequency"},
        {"iqtcp:h:12:48k:cs16:851M ", "FREQ '851M '"},
        {"iqtcp:h:12:48k:cs16:4294.9672955M", "FREQ '4294.9672955M' is above 4294.967295 MHz"},
        {"iqtcp:h:12:48k:cs16:5G", "FREQ '5G' is above"},
        {"iqtcp:h:12:48k:cs16:1:2", "too many fields"},
        {"iqudp:[::1]:12:48k:cs16", "too many fields"},
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        dsd_iq_net_spec spec;
        DSD_MEMSET(&spec, 0x5A, sizeof(spec));
        dsd_iq_net_spec untouched = spec;
        char err[512];
        const int rc = dsd_iq_net_spec_parse(rows[i].text, &spec, err, sizeof(err));
        if (rc != -1 || !strstr(err, rows[i].reason)) {
            DSD_FPRINTF(stderr, "row %zu '%.40s': rc %d, message '%s', want '%s'\n", i, rows[i].text, rc, err,
                        rows[i].reason);
            g_failures++;
        }
        CHECK(memcmp(&spec, &untouched, sizeof(spec)) == 0);
    }
    /* A field message also shows the expected shape. */
    char err[512];
    CHECK(dsd_iq_net_spec_parse("iqudp:h:12:48k:xx", NULL, err, sizeof(err)) == -1);
    dsd_iq_net_spec spec;
    CHECK(dsd_iq_net_spec_parse("iqudp:h:12:48k:xx", &spec, err, sizeof(err)) == -1);
    CHECK(strstr(err, "(expected iqudp:BIND:PORT:RATE:FORMAT[:FREQ])") != NULL);
    CHECK(dsd_iq_net_spec_parse(NULL, &spec, err, sizeof(err)) == -1);
    CHECK(dsd_iq_net_spec_kind(NULL) == DSD_IQ_NET_KIND_NONE);
}

static void
test_spec_format_refusals(void) {
    dsd_iq_net_spec spec;
    CHECK(dsd_iq_net_spec_parse("iqtcp:host:1234:48k:cs16:851M", &spec, NULL, 0) == 0);
    char text[DSD_IQ_NET_SPEC_TEXT_MAX];
    char small[16];
    CHECK(dsd_iq_net_spec_format(&spec, small, sizeof(small)) == -1);
    CHECK(small[0] == '\0');
    CHECK(dsd_iq_net_spec_format(NULL, text, sizeof(text)) == -1);
    dsd_iq_net_spec bad = spec;
    bad.port = 0;
    CHECK(dsd_iq_net_spec_format(&bad, text, sizeof(text)) == -1);
    bad = spec;
    bad.format = DSD_IQ_NET_FORMAT_NONE;
    CHECK(dsd_iq_net_spec_format(&bad, text, sizeof(text)) == -1);
    bad = spec;
    bad.rate_hz = 7999U;
    CHECK(dsd_iq_net_spec_format(&bad, text, sizeof(text)) == -1);
    bad = spec;
    DSD_SNPRINTF(bad.host, sizeof(bad.host), "::1");
    CHECK(dsd_iq_net_spec_format(&bad, text, sizeof(text)) == -1);
    /* A spec the reader takes but the text cannot carry: UDP on port 0. */
    bad = spec;
    bad.kind = DSD_IQ_NET_KIND_UDP;
    bad.port = 0;
    CHECK(iq_net_spec_usable(&bad) == 1);
    CHECK(dsd_iq_net_spec_format(&bad, text, sizeof(text)) == -1);
    bad.kind = DSD_IQ_NET_KIND_TCP;
    CHECK(iq_net_spec_usable(&bad) == 0);
}

static void
test_split_fields(void) {
    char text[] = "a::b:";
    char* fields[4];
    CHECK(iq_net_split_fields(text, fields, 4) == 4U);
    CHECK(strcmp(fields[0], "a") == 0 && fields[1][0] == '\0' && strcmp(fields[2], "b") == 0 && fields[3][0] == '\0');
    char more[] = "a:b:c";
    CHECK(iq_net_split_fields(more, fields, 2) == 3U);
}

/* ---- Wire formats ---------------------------------------------------------------------------------------------- */

static void
test_format_table(void) {
    static const struct {
        const char* name;
        size_t bytes;
        dsd_iq_net_format format;
        float clip;
    } rows[] = {
        {"cu8", 2U, DSD_IQ_NET_FORMAT_CU8, 0.996f},   {"cs8", 2U, DSD_IQ_NET_FORMAT_CS8, 0.9921875f},
        {"cs16", 4U, DSD_IQ_NET_FORMAT_CS16, 0.999f}, {"cs32", 8U, DSD_IQ_NET_FORMAT_CS32, 0.999f},
        {"cf32", 8U, DSD_IQ_NET_FORMAT_CF32, 0.999f},
    };

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        CHECK(strcmp(dsd_iq_net_format_name(rows[i].format), rows[i].name) == 0);
        CHECK(dsd_iq_net_format_from_name(rows[i].name) == rows[i].format);
        CHECK(dsd_iq_net_format_sample_bytes(rows[i].format) == rows[i].bytes);
        CHECK(float_bits_equal(dsd_iq_net_format_clip_level(rows[i].format), rows[i].clip));
    }
    CHECK(dsd_iq_net_format_from_name("CS16") == DSD_IQ_NET_FORMAT_CS16);
    CHECK(dsd_iq_net_format_from_name("cs1") == DSD_IQ_NET_FORMAT_NONE);
    CHECK(dsd_iq_net_format_from_name("") == DSD_IQ_NET_FORMAT_NONE);
    CHECK(dsd_iq_net_format_from_name(NULL) == DSD_IQ_NET_FORMAT_NONE);
    CHECK(dsd_iq_net_format_name(DSD_IQ_NET_FORMAT_NONE) == NULL);
    CHECK(dsd_iq_net_format_sample_bytes((dsd_iq_net_format)9) == 0U);
    float dst[2];
    const uint8_t src[2] = {0, 0};
    CHECK(dsd_iq_net_convert(DSD_IQ_NET_FORMAT_NONE, src, 1, dst) == 0U);
    CHECK(dsd_iq_net_convert(DSD_IQ_NET_FORMAT_CS8, NULL, 1, dst) == 0U);
}

static void
test_convert_cu8(void) {
    uint8_t src[256];
    float dst[256];
    for (int i = 0; i < 256; i++) {
        src[i] = (uint8_t)i;
    }
    CHECK(dsd_iq_net_convert(DSD_IQ_NET_FORMAT_CU8, src, 128, dst) == 128U);
    const float inv = 1.0f / 127.5f;
    for (int i = 0; i < 256; i++) {
        CHECK(float_bits_equal(dst[i], ((float)i - 127.5f) * inv));
    }
    /* The rails clip, the codes next to them do not. */
    const float clip = dsd_iq_net_format_clip_level(DSD_IQ_NET_FORMAT_CU8);
    CHECK(!(dst[0] > -clip) && !(dst[255] < clip));
    CHECK(dst[1] > -clip && dst[254] < clip);
}

static void
test_convert_cs8(void) {
    const uint8_t src[8] = {0x80, 0x7F, 0x00, 0xFF, 0x01, 0x81, 0x82, 0x7E};
    float dst[8];
    CHECK(dsd_iq_net_convert(DSD_IQ_NET_FORMAT_CS8, src, 4, dst) == 4U);
    CHECK(float_bits_equal(dst[0], -1.0f));
    CHECK(float_bits_equal(dst[1], 127.0f / 128.0f));
    CHECK(float_bits_equal(dst[2], 0.0f));
    CHECK(float_bits_equal(dst[3], -1.0f / 128.0f));
    CHECK(float_bits_equal(dst[4], 1.0f / 128.0f));
    CHECK(float_bits_equal(dst[5], -127.0f / 128.0f));
    CHECK(float_bits_equal(dst[6], -126.0f / 128.0f));
    CHECK(float_bits_equal(dst[7], 126.0f / 128.0f));
    /* Both rails clip (the magnitude test also takes -127, the mirror of the positive rail); +-126 do not. */
    const float clip = dsd_iq_net_format_clip_level(DSD_IQ_NET_FORMAT_CS8);
    CHECK(!(dst[0] > -clip) && !(dst[1] < clip) && !(dst[5] > -clip));
    CHECK(dst[6] > -clip && dst[7] < clip);
}

static void
test_convert_cs16(void) {
    const uint8_t src[12] = {0x00, 0x80, 0xFF, 0x7F, 0x01, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x34, 0x12};
    float dst[6];
    CHECK(dsd_iq_net_convert(DSD_IQ_NET_FORMAT_CS16, src, 3, dst) == 3U);
    CHECK(float_bits_equal(dst[0], -1.0f));
    CHECK(float_bits_equal(dst[1], 32767.0f / 32768.0f));
    CHECK(float_bits_equal(dst[2], 1.0f / 32768.0f));
    CHECK(float_bits_equal(dst[3], -1.0f / 32768.0f));
    CHECK(float_bits_equal(dst[4], 0.0f));
    CHECK(float_bits_equal(dst[5], 4660.0f / 32768.0f));
}

static void
test_convert_cs32(void) {
    uint8_t src[24];
    put_le32(src + 0, 0x80000000U);
    put_le32(src + 4, 0x7FFFFFFFU);
    put_le32(src + 8, 1U);
    put_le32(src + 12, 0xFFFFFFFFU);
    put_le32(src + 16, 0x12345678U);
    put_le32(src + 20, 0xC0000000U);
    float dst[6];
    CHECK(dsd_iq_net_convert(DSD_IQ_NET_FORMAT_CS32, src, 3, dst) == 3U);
    const float inv = 1.0f / 2147483648.0f;
    CHECK(float_bits_equal(dst[0], -1.0f));
    CHECK(float_bits_equal(dst[1], (float)2147483647 * inv)); /* rounds to 1.0f, as the WAV s32 rule does */
    CHECK(float_bits_equal(dst[1], 1.0f));
    CHECK(float_bits_equal(dst[2], inv));
    CHECK(float_bits_equal(dst[3], -inv));
    CHECK(float_bits_equal(dst[4], (float)0x12345678 * inv));
    CHECK(float_bits_equal(dst[5], -0.5f));
}

static void
test_convert_cf32_bounded(void) {
    /* Bits of: in range, negative, above full scale (passes unclamped), NaN, +Inf, -Inf, 2^60, -2^60, the float just
       below 2^60, 1e30, a negative NaN, -0.0. */
    static const uint32_t in_bits[12] = {0x3F000000U, 0xBE800000U, 0x3FC00000U, 0x7FC00000U, 0x7F800000U, 0xFF800000U,
                                         0x5D800000U, 0xDD800000U, 0x5D7FFFFFU, 0x7149F2CAU, 0xFFC00001U, 0x80000000U};
    static const uint32_t want_bits[12] = {0x3F000000U, 0xBE800000U, 0x3FC00000U, 0U, 0U, 0U,
                                           0U,          0U,          0x5D7FFFFFU, 0U, 0U, 0x80000000U};
    uint8_t raw[1 + 48];
    for (int i = 0; i < 12; i++) {
        put_le32(raw + 1 + 4 * (size_t)i, in_bits[i]); /* at an odd address: any alignment */
    }
    float dst[12];
    float ref[12];
    CHECK(dsd_iq_net_convert(DSD_IQ_NET_FORMAT_CF32, raw + 1, 6, dst) == 6U);
    bound_cf32_to_f32(raw + 1, ref, 6);
    for (int i = 0; i < 12; i++) {
        CHECK(float_bits_equal(dst[i], float_from_bits(want_bits[i])));
        CHECK(float_bits_equal(dst[i], ref[i]));
    }
}

/* Every split of a byte stream into two reads, and one-byte reads, give the samples one read gives. */
static void
test_carry_every_split(void) {
    static const dsd_iq_net_format formats[] = {DSD_IQ_NET_FORMAT_CU8, DSD_IQ_NET_FORMAT_CS16, DSD_IQ_NET_FORMAT_CS32,
                                                DSD_IQ_NET_FORMAT_CF32};

    enum { k_samples = 5, k_floats = 2 * k_samples };

    for (size_t f = 0; f < sizeof(formats) / sizeof(formats[0]); f++) {
        const size_t sb = dsd_iq_net_format_sample_bytes(formats[f]);
        const size_t total = sb * k_samples;
        uint8_t stream[8 * k_samples];
        for (size_t i = 0; i < total; i++) {
            stream[i] = (uint8_t)(0x31U + 7U * i);
        }
        float want[k_floats];
        CHECK(dsd_iq_net_convert(formats[f], stream, k_samples, want) == k_samples);
        for (size_t cut = 0; cut <= total; cut++) {
            for (int one_byte = 0; one_byte < 2; one_byte++) {
                iq_net_carry carry = {{0}, 0};
                float got[k_floats];
                DSD_MEMSET(got, 0, sizeof(got));
                size_t got_samples = 0;
                size_t pos = 0;
                while (pos < total) {
                    size_t len = pos < cut ? cut - pos : total - pos;
                    if (one_byte) {
                        len = 1U;
                    }
                    uint8_t buf[8 + 8 * k_samples];
                    const size_t head = iq_net_carry_restore(&carry, buf);
                    DSD_MEMCPY(buf + head, stream + pos, len);
                    const size_t whole = iq_net_carry_keep(&carry, buf, head + len, sb);
                    (void)dsd_iq_net_convert(formats[f], buf, whole, got + 2U * got_samples);
                    got_samples += whole;
                    pos += len;
                }
                CHECK(got_samples == k_samples);
                CHECK(carry.len == 0U);
                for (size_t i = 0; i < k_floats; i++) {
                    CHECK(float_bits_equal(got[i], want[i]));
                }
            }
        }
    }
}

/* ---- Rate telemetry -------------------------------------------------------------------------------------------- */

/* Feed one window's samples in bursts of 1/5 of the window each, starting at *t. */
static int
rate_window(iq_net_rate_monitor* m, uint64_t* t, uint64_t samples, uint64_t* measured) {
    int warn = 0;
    for (int i = 0; i < 5; i++) {
        warn |= iq_net_rate_update(m, *t, samples / 5U, measured);
        *t += IQ_NET_RATE_WINDOW_MS / 5U;
    }
    return warn;
}

static int
rate_run(iq_net_rate_monitor* m, uint64_t* t, uint64_t samples, int windows, uint64_t* measured) {
    int warns = 0;
    for (int w = 0; w < windows; w++) {
        warns += rate_window(m, t, samples, measured);
    }
    warns += iq_net_rate_update(m, *t, 0, measured);
    return warns;
}

static void
test_rate_rows(void) {
    const uint32_t rate = 48000U;
    const uint64_t exact = (uint64_t)rate * 10U; /* samples in one window at the rate */
    iq_net_rate_monitor m;
    uint64_t t = 1000;
    uint64_t measured = 0;

    /* Correct rate in bursts: no warning. */
    iq_net_rate_arm(&m, rate, t);
    t += IQ_NET_RATE_WARMUP_MS;
    CHECK(rate_run(&m, &t, exact, 8, &measured) == 0);

    /* Catch-up: a short window then a long one, over and over (a host pause the sender's backlog makes up). */
    iq_net_rate_arm(&m, rate, t);
    t += IQ_NET_RATE_WARMUP_MS;
    for (int i = 0; i < 4; i++) {
        CHECK(rate_run(&m, &t, exact * 80U / 100U, 1, &measured) == 0);
        CHECK(rate_run(&m, &t, exact * 120U / 100U, 1, &measured) == 0);
    }

    /* Silence: the reader pauses the monitor at the stall; nothing warns however long it lasts. */
    iq_net_rate_arm(&m, rate, t);
    t += IQ_NET_RATE_WARMUP_MS;
    iq_net_rate_pause(&m);
    CHECK(rate_run(&m, &t, 0, 10, &measured) == 0);

    /* 15 % low: one warning after three windows, with the mean rate, and no more on this connection. */
    iq_net_rate_arm(&m, rate, t);
    t += IQ_NET_RATE_WARMUP_MS;
    CHECK(rate_run(&m, &t, exact * 85U / 100U, 2, &measured) == 0);
    measured = 0;
    CHECK(rate_run(&m, &t, exact * 85U / 100U, 1, &measured) == 1);
    CHECK(measured == 40800U);
    CHECK(rate_run(&m, &t, exact * 85U / 100U, 6, &measured) == 0);

    /* Re-armed by a reconnect: warns again, 15 % high this time. */
    iq_net_rate_arm(&m, rate, t);
    t += IQ_NET_RATE_WARMUP_MS;
    measured = 0;
    CHECK(rate_run(&m, &t, exact * 115U / 100U, 3, &measured) == 1);
    CHECK(measured == 55200U);

    /* Exactly 10 % off either way: never. */
    iq_net_rate_arm(&m, rate, t);
    t += IQ_NET_RATE_WARMUP_MS;
    CHECK(rate_run(&m, &t, exact * 90U / 100U, 5, &measured) == 0);
    CHECK(rate_run(&m, &t, exact * 110U / 100U, 5, &measured) == 0);
    /* One sample past the bound is outside it. */
    CHECK(rate_run(&m, &t, exact * 110U / 100U + 5U, 3, &measured) == 1);

    /* The warm-up does not count: two off windows' worth during it, then two off windows, then a good one. */
    iq_net_rate_arm(&m, rate, t);
    CHECK(rate_window(&m, &t, exact / 4U, &measured) == 0); /* 5 s of warm-up plus half of the first window */
    t -= IQ_NET_RATE_WINDOW_MS / 2U;
    CHECK(rate_run(&m, &t, exact / 4U, 2, &measured) == 0);
    CHECK(rate_run(&m, &t, exact, 1, &measured) == 0);

    /* A discarded window (boundary, stall, known loss, drop) breaks a streak; three clean ones after it warn. */
    iq_net_rate_arm(&m, rate, t);
    t += IQ_NET_RATE_WARMUP_MS;
    CHECK(rate_run(&m, &t, exact / 2U, 2, &measured) == 0);
    iq_net_rate_discard(&m);
    CHECK(rate_run(&m, &t, exact / 2U, 1, &measured) == 0);
    CHECK(rate_run(&m, &t, exact / 2U, 2, &measured) == 0);
    CHECK(rate_run(&m, &t, exact / 2U, 1, &measured) == 1);

    /* Sides do not mix: low, high, low never makes a streak. */
    iq_net_rate_arm(&m, rate, t);
    t += IQ_NET_RATE_WARMUP_MS;
    for (int i = 0; i < 3; i++) {
        CHECK(rate_run(&m, &t, exact / 2U, 1, &measured) == 0);
        CHECK(rate_run(&m, &t, exact * 2U, 1, &measured) == 0);
    }

    /* A restart after a resume warms up again, but the warning stays given for the connection. */
    iq_net_rate_arm(&m, rate, t);
    t += IQ_NET_RATE_WARMUP_MS;
    CHECK(rate_run(&m, &t, exact / 2U, 3, &measured) == 1);
    iq_net_rate_pause(&m);
    iq_net_rate_restart(&m, t);
    t += IQ_NET_RATE_WARMUP_MS;
    CHECK(rate_run(&m, &t, exact / 2U, 5, &measured) == 0);

    char text[256];
    iq_net_rate_warning_text(text, sizeof(text), 55200U, 48000U);
    CHECK(strcmp(text,
                 "I/Q stream: about 55200 samples/s are arriving, but the input is set to 48000. If nothing decodes, "
                 "check that the sample rate matches the sender.\n")
          == 0);
}

/* ---- Reader harness -------------------------------------------------------------------------------------------- */

enum { k_rec_floats = 1 << 16, k_rec_events = 64 };

typedef struct {
    dsd_mutex_t mu;
    float iq[k_rec_floats]; /* the first k_rec_floats received */
    size_t floats;          /* all received */
    int events[k_rec_events];
    size_t event_at[k_rec_events]; /* floats received before the event */
    char peers[k_rec_events][32];
    size_t lost_bytes[k_rec_events];
    size_t nevents;
    dsd_iq_net_reader* reader; /* note_written() after each batch, as the stream would after a chunk */
} recorder;

static dsd_atomic_u64 g_clock_ms;

static uint64_t
fake_clock(void* ctx) {
    (void)ctx;
    return dsd_atomic_u64_load_acquire(&g_clock_ms);
}

static void
advance_clock(uint64_t ms) {
    dsd_atomic_u64_store_release(&g_clock_ms, dsd_atomic_u64_load_acquire(&g_clock_ms) + ms);
}

static void
rec_samples(void* ctx, const float* iq, size_t pairs) {
    recorder* rec = (recorder*)ctx;
    (void)dsd_mutex_lock(&rec->mu);
    for (size_t i = 0; i < 2U * pairs; i++) {
        if (rec->floats < k_rec_floats) {
            rec->iq[rec->floats] = iq[i];
        }
        rec->floats++;
    }
    (void)dsd_mutex_unlock(&rec->mu);
    dsd_iq_net_reader_note_written(rec->reader);
}

static void
rec_event(void* ctx, const dsd_iq_net_event* ev) {
    recorder* rec = (recorder*)ctx;
    (void)dsd_mutex_lock(&rec->mu);
    if (rec->nevents < k_rec_events) {
        rec->events[rec->nevents] = (int)ev->kind;
        rec->event_at[rec->nevents] = rec->floats;
        DSD_SNPRINTF(rec->peers[rec->nevents], sizeof(rec->peers[0]), "%s", ev->peer ? ev->peer : "");
        rec->lost_bytes[rec->nevents] = ev->bytes;
        rec->nevents++;
    }
    (void)dsd_mutex_unlock(&rec->mu);
}

static size_t
rec_floats(recorder* rec) {
    (void)dsd_mutex_lock(&rec->mu);
    const size_t n = rec->floats;
    (void)dsd_mutex_unlock(&rec->mu);
    return n;
}

static size_t
rec_count_event(recorder* rec, dsd_iq_net_event_kind kind) {
    size_t n = 0;
    (void)dsd_mutex_lock(&rec->mu);
    for (size_t i = 0; i < rec->nevents; i++) {
        n += rec->events[i] == (int)kind;
    }
    (void)dsd_mutex_unlock(&rec->mu);
    return n;
}

/* The index of the n-th (0-based) event of kind, or -1. */
static int
rec_find_event(recorder* rec, dsd_iq_net_event_kind kind, size_t nth) {
    int at = -1;
    (void)dsd_mutex_lock(&rec->mu);
    for (size_t i = 0; i < rec->nevents; i++) {
        if (rec->events[i] == (int)kind && nth-- == 0) {
            at = (int)i;
            break;
        }
    }
    (void)dsd_mutex_unlock(&rec->mu);
    return at;
}

static int
wait_floats(recorder* rec, size_t want) {
    const uint64_t until = dsd_realtime_mono_ms() + k_wait_ms;
    while (rec_floats(rec) < want) {
        if (dsd_realtime_mono_ms() > until) {
            DSD_FPRINTF(stderr, "timed out waiting for %zu floats (have %zu)\n", want, rec_floats(rec));
            return 0;
        }
        dsd_sleep_ms(2U);
    }
    return 1;
}

static int
wait_event(recorder* rec, dsd_iq_net_event_kind kind, size_t count) {
    const uint64_t until = dsd_realtime_mono_ms() + k_wait_ms;
    while (rec_count_event(rec, kind) < count) {
        if (dsd_realtime_mono_ms() > until) {
            DSD_FPRINTF(stderr, "timed out waiting for event %d x%zu\n", (int)kind, count);
            return 0;
        }
        dsd_sleep_ms(2U);
    }
    return 1;
}

static int
wait_counter(dsd_iq_net_reader* r, size_t offset, uint64_t want) {
    const uint64_t until = dsd_realtime_mono_ms() + k_wait_ms;
    for (;;) {
        dsd_iq_net_counters c;
        dsd_iq_net_reader_get_counters(r, &c);
        uint64_t v = 0;
        DSD_MEMCPY(&v, (const uint8_t*)&c + offset, sizeof(v));
        if (v >= want) {
            return 1;
        }
        if (dsd_realtime_mono_ms() > until) {
            DSD_FPRINTF(stderr, "timed out waiting for a counter to reach %llu\n", (unsigned long long)want);
            return 0;
        }
        dsd_sleep_ms(2U);
    }
}

#define WAIT_COUNTER(r, field, want) wait_counter((r), offsetof(dsd_iq_net_counters, field), (want))

static recorder*
rec_new(void) {
    recorder* rec = (recorder*)calloc(1, sizeof(*rec));
    if (rec) {
        (void)dsd_mutex_init(&rec->mu);
    }
    return rec;
}

static void
rec_free(recorder* rec) {
    if (rec) {
        (void)dsd_mutex_destroy(&rec->mu);
        free(rec);
    }
}

static dsd_iq_net_reader*
reader_new(dsd_iq_net_kind kind, int port, dsd_iq_net_format format, uint32_t rate_hz, recorder* rec) {
    dsd_iq_net_spec spec;
    DSD_MEMSET(&spec, 0, sizeof(spec));
    spec.kind = kind;
    DSD_SNPRINTF(spec.host, sizeof(spec.host), "127.0.0.1");
    spec.port = port;
    spec.rate_hz = rate_hz;
    spec.format = format;
    dsd_iq_net_reader_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    cfg.on_samples = rec_samples;
    cfg.on_event = rec_event;
    cfg.ctx = rec;
    cfg.clock_ms = fake_clock;
    dsd_iq_net_reader* r = dsd_iq_net_reader_create(&spec, &cfg);
    rec->reader = r;
    return r;
}

static int
local_port(dsd_socket_t sock) {
    if (sock == DSD_INVALID_SOCKET) {
        return -1;
    }
    struct sockaddr_in sa;
#if DSD_PLATFORM_WIN_NATIVE
    int len = (int)sizeof(sa);
#else
    socklen_t len = (socklen_t)sizeof(sa);
#endif
    DSD_MEMSET(&sa, 0, sizeof(sa));
    if (getsockname(sock, (struct sockaddr*)&sa, &len) != 0) {
        return -1;
    }
    return (int)ntohs(sa.sin_port);
}

static void
loopback_addr(struct sockaddr_in* sa, int port) {
    DSD_MEMSET(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa->sin_port = htons((uint16_t)port);
}

/* A socket bound to 127.0.0.1 on a free port: a TCP listener when listen_backlog >= 0, else bound only. */
static dsd_socket_t
bound_socket(int type, int listen_backlog, int* port) {
    dsd_socket_t s = dsd_socket_create(AF_INET, type, 0);
    if (s == DSD_INVALID_SOCKET) {
        return s;
    }
    struct sockaddr_in sa;
    loopback_addr(&sa, 0);
    if (dsd_socket_bind(s, (struct sockaddr*)&sa, (int)sizeof(sa)) != 0
        || (listen_backlog >= 0 && dsd_socket_listen(s, listen_backlog) != 0)) {
        (void)dsd_socket_close(s);
        return DSD_INVALID_SOCKET;
    }
    *port = local_port(s);
    return s;
}

static dsd_socket_t
accept_within(dsd_socket_t listener, unsigned ms) {
    if (dsd_socket_wait(listener, DSD_SOCKET_WAIT_READ, ms) <= 0) {
        return DSD_INVALID_SOCKET;
    }
    dsd_socket_t s = dsd_socket_accept(listener, NULL, NULL);
    if (s != DSD_INVALID_SOCKET) {
#ifdef SO_NOSIGPIPE
        const int one = 1;
        (void)dsd_socket_setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, (int)sizeof(one));
#endif
        const int nodelay = 1;
        (void)dsd_socket_setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &nodelay, (int)sizeof(nodelay));
    }
    return s;
}

static int
send_all(dsd_socket_t s, const uint8_t* p, size_t n) {
    while (n > 0) {
        const int k = dsd_socket_send(s, p, n, TEST_SEND_FLAGS);
        if (k <= 0) {
            return -1;
        }
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

/* Open r (open runs connect against a listener whose accept the caller makes next). */
static int
open_ok(dsd_iq_net_reader* r) {
    dsd_iq_net_open_error err;
    if (!r || dsd_iq_net_reader_open(r, &err) != 0) {
        DSD_FPRINTF(stderr, "open failed: %s\n", r ? err.message : "no reader");
        return 0;
    }
    return 1;
}

static uint64_t
stop_ms(dsd_iq_net_reader* r) {
    const uint64_t t0 = dsd_realtime_mono_ms();
    dsd_iq_net_reader_stop(r);
    return dsd_realtime_mono_ms() - t0;
}

/* ---- TCP ------------------------------------------------------------------------------------------------------- */

/* Each format's bytes, written one byte per send, arrive as the samples one conversion gives. */
static void
test_tcp_one_byte_writes(void) {
    static const dsd_iq_net_format formats[] = {DSD_IQ_NET_FORMAT_CS8, DSD_IQ_NET_FORMAT_CS16, DSD_IQ_NET_FORMAT_CF32};
    for (size_t f = 0; f < sizeof(formats) / sizeof(formats[0]); f++) {
        int port = 0;
        dsd_socket_t listener = bound_socket(SOCK_STREAM, 4, &port);
        recorder* rec = rec_new();
        dsd_iq_net_reader* r = reader_new(DSD_IQ_NET_KIND_TCP, port, formats[f], 48000U, rec);
        CHECK(listener != DSD_INVALID_SOCKET && rec && open_ok(r));
        dsd_socket_t conn = accept_within(listener, k_wait_ms);
        CHECK(conn != DSD_INVALID_SOCKET);
        CHECK(dsd_iq_net_reader_start(r) == 0);
        CHECK(dsd_iq_net_reader_start(r) == -1);

        enum { k_samples = 9, k_floats = 2 * k_samples };

        const size_t sb = dsd_iq_net_format_sample_bytes(formats[f]);
        uint8_t bytes[8 * k_samples];
        for (size_t i = 0; i < sb * k_samples; i++) {
            bytes[i] = (uint8_t)(0x11U + 13U * i);
        }
        for (size_t i = 0; i < sb * k_samples; i++) {
            CHECK(send_all(conn, bytes + i, 1) == 0);
            if (i % 3U == 0U) {
                dsd_sleep_ms(1U);
            }
        }
        float want[k_floats];
        (void)dsd_iq_net_convert(formats[f], bytes, k_samples, want);
        CHECK(wait_floats(rec, k_floats));
        (void)dsd_mutex_lock(&rec->mu);
        CHECK(rec->floats == k_floats);
        for (size_t i = 0; i < k_floats && i < rec->floats; i++) {
            CHECK(float_bits_equal(rec->iq[i], want[i]));
        }
        char peer[32];
        DSD_SNPRINTF(peer, sizeof(peer), "127.0.0.1:%d", port);
        CHECK(rec->nevents == 1U && rec->events[0] == DSD_IQ_NET_EVENT_CONNECTED && strcmp(rec->peers[0], peer) == 0);
        CHECK(dsd_iq_net_reader_spec(r)->port == port);
        (void)dsd_mutex_unlock(&rec->mu);
        CHECK(dsd_iq_net_reader_available(r) == 1);
        dsd_iq_net_counters c;
        dsd_iq_net_reader_get_counters(r, &c);
        CHECK(c.samples == k_samples && c.bytes == sb * k_samples && c.connect_attempts == 1U);
        dsd_iq_net_reader_destroy(r);
        (void)dsd_socket_close(conn);
        (void)dsd_socket_close(listener);
        rec_free(rec);
    }
}

/* A partial sample cut off by a lost connection is not glued to the next connection's bytes. */
static void
test_tcp_carry_discarded_on_reconnect(void) {
    int port = 0;
    dsd_socket_t listener = bound_socket(SOCK_STREAM, 4, &port);
    recorder* rec = rec_new();
    dsd_iq_net_reader* r = reader_new(DSD_IQ_NET_KIND_TCP, port, DSD_IQ_NET_FORMAT_CS16, 48000U, rec);
    CHECK(listener != DSD_INVALID_SOCKET && rec && open_ok(r));
    dsd_socket_t conn = accept_within(listener, k_wait_ms);
    CHECK(dsd_iq_net_reader_start(r) == 0);
    const uint8_t first[6] = {0x00, 0x40, 0x00, 0xC0, 0x7F, 0x7F}; /* one sample and half of another */
    CHECK(send_all(conn, first, sizeof(first)) == 0);
    CHECK(wait_floats(rec, 2U));
    CHECK(WAIT_COUNTER(r, bytes, 6U));
    (void)dsd_socket_close(conn);
    conn = accept_within(listener, k_wait_ms);
    CHECK(conn != DSD_INVALID_SOCKET);
    const uint8_t second[8] = {0x00, 0x20, 0x00, 0xE0, 0x00, 0x10, 0x00, 0xF0};
    CHECK(send_all(conn, second, sizeof(second)) == 0);
    CHECK(wait_floats(rec, 6U));
    static const float want[6] = {0.5f, -0.5f, 0.25f, -0.25f, 0.125f, -0.125f};
    (void)dsd_mutex_lock(&rec->mu);
    for (size_t i = 0; i < 6U && i < rec->floats; i++) {
        CHECK(float_bits_equal(rec->iq[i], want[i]));
    }
    CHECK(rec->nevents == 3U);
    CHECK(rec->events[0] == DSD_IQ_NET_EVENT_CONNECTED);
    CHECK(rec->events[1] == DSD_IQ_NET_EVENT_DISCONNECTED);
    CHECK(rec->events[2] == DSD_IQ_NET_EVENT_RECONNECT && rec->event_at[2] == 2U);
    (void)dsd_mutex_unlock(&rec->mu);
    dsd_iq_net_counters c;
    dsd_iq_net_reader_get_counters(r, &c);
    CHECK(c.carry_discarded_bytes == 2U && c.reconnects == 1U && c.samples == 3U);
    dsd_iq_net_reader_destroy(r);
    (void)dsd_socket_close(conn);
    (void)dsd_socket_close(listener);
    rec_free(rec);
}

/* A quiet connection stalls (it is not lost) and resumes; a close is a loss. Availability follows. */
static void
test_tcp_stall_vs_close(void) {
    int port = 0;
    dsd_socket_t listener = bound_socket(SOCK_STREAM, 4, &port);
    recorder* rec = rec_new();
    dsd_iq_net_reader* r = reader_new(DSD_IQ_NET_KIND_TCP, port, DSD_IQ_NET_FORMAT_CS16, 48000U, rec);
    rec->reader = NULL; /* this test moves the state to STREAMING by hand */
    CHECK(listener != DSD_INVALID_SOCKET && rec && open_ok(r));
    CHECK(dsd_iq_net_reader_state(r) == DSD_IQ_NET_STATE_WAITING);
    dsd_socket_t conn = accept_within(listener, k_wait_ms);
    CHECK(dsd_iq_net_reader_start(r) == 0);
    const uint8_t sample[5] = {0x00, 0x40, 0x00, 0x40, 0x12}; /* a sample and a byte of the next */
    CHECK(send_all(conn, sample, sizeof(sample)) == 0);
    CHECK(wait_floats(rec, 2U));
    CHECK(dsd_iq_net_reader_available(r) == 0);
    dsd_iq_net_reader_note_written(r);
    CHECK(dsd_iq_net_reader_available(r) == 1);

    advance_clock(DSD_IQ_NET_STALL_MS - 1U);
    dsd_sleep_ms(3U * DSD_IQ_NET_WAIT_SLICE_MS / 2U);
    CHECK(rec_count_event(rec, DSD_IQ_NET_EVENT_STALL) == 0U);
    advance_clock(1U);
    CHECK(wait_event(rec, DSD_IQ_NET_EVENT_STALL, 1U));
    CHECK(dsd_iq_net_reader_state(r) == DSD_IQ_NET_STATE_STALLED);
    CHECK(dsd_iq_net_reader_available(r) == 0);
    dsd_iq_net_reader_note_written(r); /* not a resume */
    CHECK(dsd_iq_net_reader_state(r) == DSD_IQ_NET_STATE_STALLED);
    CHECK(rec_count_event(rec, DSD_IQ_NET_EVENT_DISCONNECTED) == 0U);

    /* The byte carried across the stall completes the next sample. */
    const uint8_t rest[3] = {0x34, 0x00, 0xC0};
    CHECK(send_all(conn, rest, sizeof(rest)) == 0);
    CHECK(wait_floats(rec, 4U));
    const int resume = rec_find_event(rec, DSD_IQ_NET_EVENT_RESUME, 0);
    CHECK(resume >= 0 && rec->event_at[resume] == 2U);
    (void)dsd_mutex_lock(&rec->mu);
    CHECK(float_bits_equal(rec->iq[2], 13330.0f / 32768.0f) && float_bits_equal(rec->iq[3], -0.5f));
    (void)dsd_mutex_unlock(&rec->mu);
    CHECK(dsd_iq_net_reader_state(r) == DSD_IQ_NET_STATE_WAITING);
    dsd_iq_net_reader_note_written(r);
    CHECK(dsd_iq_net_reader_available(r) == 1);

    (void)dsd_socket_close(conn);
    CHECK(wait_event(rec, DSD_IQ_NET_EVENT_DISCONNECTED, 1U));
    CHECK(dsd_iq_net_reader_available(r) == 0);
    conn = accept_within(listener, k_wait_ms);
    CHECK(wait_event(rec, DSD_IQ_NET_EVENT_RECONNECT, 1U));
    dsd_iq_net_counters c;
    dsd_iq_net_reader_get_counters(r, &c);
    CHECK(c.stalls == 1U && c.resumes == 1U && c.reconnects == 1U && c.rate_warnings == 0U);
    CHECK(stop_ms(r) < k_prompt_ms);
    CHECK(dsd_iq_net_reader_state(r) == DSD_IQ_NET_STATE_STOPPED);
    dsd_iq_net_reader_destroy(r);
    (void)dsd_socket_close(conn);
    (void)dsd_socket_close(listener);
    rec_free(rec);
}

/* Samples that come back across a stall's deadline while a wait slice runs: the slice began before the deadline, so
   no tick saw the stall, and the samples are checked again before they go out. The pause is a stall and a resume (the
   boundary), not samples that follow on. Before, the late samples hid the pause for good. */
static void
test_tcp_stall_seen_after_the_wait(void) {
    int port = 0;
    dsd_socket_t listener = bound_socket(SOCK_STREAM, 4, &port);
    recorder* rec = rec_new();
    dsd_iq_net_reader* r = reader_new(DSD_IQ_NET_KIND_TCP, port, DSD_IQ_NET_FORMAT_CS16, 48000U, rec);
    CHECK(listener != DSD_INVALID_SOCKET && rec && open_ok(r));
    dsd_socket_t conn = accept_within(listener, k_wait_ms);
    CHECK(dsd_iq_net_reader_start(r) == 0);
    const uint8_t one[4] = {0x00, 0x40, 0x00, 0xC0};
    CHECK(send_all(conn, one, sizeof(one)) == 0);
    CHECK(wait_floats(rec, 2U));
    dsd_sleep_ms(DSD_IQ_NET_WAIT_SLICE_MS / 4U); /* the reader is inside its next wait */
    advance_clock(DSD_IQ_NET_STALL_MS + 1U);
    CHECK(send_all(conn, one, sizeof(one)) == 0);
    CHECK(wait_floats(rec, 4U));
    CHECK(rec_count_event(rec, DSD_IQ_NET_EVENT_STALL) == 1U);
    const int resume = rec_find_event(rec, DSD_IQ_NET_EVENT_RESUME, 0);
    CHECK(resume >= 0 && rec->event_at[resume] == 2U);
    dsd_iq_net_reader_destroy(r);
    (void)dsd_socket_close(conn);
    (void)dsd_socket_close(listener);
    rec_free(rec);
}

typedef struct {
    dsd_socket_t listener;
    atomic_int stop;
    atomic_int accepts;
} closing_server;

/* Accept and close at once: every connection is lost before it delivers a sample. */
static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    closing_server_main(void* arg) {
    closing_server* srv = (closing_server*)arg;
    while (!atomic_load(&srv->stop)) {
        dsd_socket_t s = accept_within(srv->listener, 20U);
        if (s != DSD_INVALID_SOCKET) {
            (void)atomic_fetch_add(&srv->accepts, 1);
            (void)dsd_socket_close(s);
        }
    }
    DSD_THREAD_RETURN;
}

/* Connections that end before any sample back off (they do not reset the attempt count), and a stop during the
   backoff is prompt. */
static void
test_tcp_backoff_cancellable(void) {
    closing_server srv;
    int port = 0;
    srv.listener = bound_socket(SOCK_STREAM, 8, &port);
    atomic_init(&srv.stop, 0);
    atomic_init(&srv.accepts, 0);
    recorder* rec = rec_new();
    dsd_iq_net_reader* r = reader_new(DSD_IQ_NET_KIND_TCP, port, DSD_IQ_NET_FORMAT_CU8, 48000U, rec);
    CHECK(srv.listener != DSD_INVALID_SOCKET && rec && open_ok(r));
    dsd_thread_t thread;
    CHECK(dsd_thread_create(&thread, closing_server_main, &srv) == 0);
    CHECK(dsd_iq_net_reader_start(r) == 0);
    const uint64_t t0 = dsd_realtime_mono_ms();
    /* Attempt 1 at once, 2 after 200 ms, 3 after 400 ms more: the third connect comes no sooner than 600 ms in. */
    CHECK(WAIT_COUNTER(r, connect_attempts, 4U));
    CHECK(dsd_realtime_mono_ms() - t0 >= 550U);
    /* Attempt 4 waits 600 ms, 5 waits 800 ms: stop inside the latter. */
    CHECK(WAIT_COUNTER(r, connect_attempts, 5U));
    dsd_sleep_ms(100U);
    CHECK(stop_ms(r) < k_prompt_ms / 3U);
    CHECK(dsd_iq_net_reader_state(r) == DSD_IQ_NET_STATE_STOPPED);
    CHECK(rec_count_event(rec, DSD_IQ_NET_EVENT_STALL) == 0U);
    atomic_store(&srv.stop, 1);
    (void)dsd_thread_join(thread);
    dsd_iq_net_reader_destroy(r);
    (void)dsd_socket_close(srv.listener);
    rec_free(rec);
}

/* A stop while the reader waits on a silent connection returns within about one wait slice. */
static void
test_tcp_stop_during_recv(void) {
    int port = 0;
    dsd_socket_t listener = bound_socket(SOCK_STREAM, 4, &port);
    recorder* rec = rec_new();
    dsd_iq_net_reader* r = reader_new(DSD_IQ_NET_KIND_TCP, port, DSD_IQ_NET_FORMAT_CS16, 48000U, rec);
    CHECK(listener != DSD_INVALID_SOCKET && rec && open_ok(r));
    dsd_socket_t conn = accept_within(listener, k_wait_ms);
    CHECK(dsd_iq_net_reader_start(r) == 0);
    CHECK(wait_event(rec, DSD_IQ_NET_EVENT_CONNECTED, 1U));
    dsd_sleep_ms(50U);
    CHECK(stop_ms(r) < k_prompt_ms / 3U);
    dsd_iq_net_reader_stop(r); /* idempotent */
    CHECK(rec_count_event(rec, DSD_IQ_NET_EVENT_DISCONNECTED) == 0U);
    CHECK(dsd_iq_net_reader_local_port(r) == -1);
    dsd_iq_net_reader_destroy(r);
    (void)dsd_socket_close(conn);
    (void)dsd_socket_close(listener);
    rec_free(rec);
}

typedef struct {
    dsd_iq_net_reader* reader;
    atomic_int done;
    int rc;
    dsd_iq_net_open_error err;
} opener;

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    opener_main(void* arg) {
    opener* o = (opener*)arg;
    o->rc = dsd_iq_net_reader_open(o->reader, &o->err);
    atomic_store(&o->done, 1);
    DSD_THREAD_RETURN;
}

/* A stop cancels a connect that would otherwise wait out its bound. The server's accept queue is full, so the
   handshake gets no answer (where the platform drops the SYN; elsewhere the connect fails at once). */
static void
test_tcp_stop_during_connect(void) {
    int port = 0;
    dsd_socket_t listener = bound_socket(SOCK_STREAM, 0, &port);
    CHECK(listener != DSD_INVALID_SOCKET);

    enum { k_fillers = 8 };

    dsd_socket_t fillers[k_fillers];
    struct sockaddr_in sa;
    loopback_addr(&sa, port);
    for (int i = 0; i < k_fillers; i++) {
        fillers[i] = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
        (void)dsd_socket_set_nonblocking(fillers[i], 1);
        (void)dsd_socket_connect(fillers[i], (struct sockaddr*)&sa, (int)sizeof(sa));
    }
    dsd_sleep_ms(100U);
    recorder* rec = rec_new();
    opener o;
    DSD_MEMSET(&o, 0, sizeof(o));
    o.reader = reader_new(DSD_IQ_NET_KIND_TCP, port, DSD_IQ_NET_FORMAT_CS16, 48000U, rec);
    atomic_init(&o.done, 0);
    dsd_thread_t thread;
    CHECK(dsd_thread_create(&thread, opener_main, &o) == 0);
    dsd_sleep_ms(300U);
    const int still_connecting = !atomic_load(&o.done);
    const uint64_t t0 = dsd_realtime_mono_ms();
    dsd_iq_net_reader_stop(o.reader);
    while (!atomic_load(&o.done) && dsd_realtime_mono_ms() - t0 < (uint64_t)k_wait_ms) {
        dsd_sleep_ms(2U);
    }
    CHECK(dsd_realtime_mono_ms() - t0 < k_prompt_ms / 3U);
    (void)dsd_thread_join(thread);
    if (still_connecting) {
        CHECK(o.rc == -1);
        CHECK(dsd_iq_net_reader_state(o.reader) == DSD_IQ_NET_STATE_STOPPED);
    }
    DSD_FPRINTF(stderr, "stop_during_connect: connect %s when stopped\n",
                still_connecting ? "was waiting" : "had already ended");
    dsd_iq_net_reader_destroy(o.reader);
    for (int i = 0; i < k_fillers; i++) {
        (void)dsd_socket_close(fillers[i]);
    }
    (void)dsd_socket_close(listener);
    rec_free(rec);
}

/* Nothing listens on the port: open fails as refused, names the server, and the reader stays stopped. */
static void
test_tcp_connect_refused(void) {
    int port = 0;
    dsd_socket_t holder = bound_socket(SOCK_STREAM, -1, &port); /* bound, not listening: the port stays ours */
    recorder* rec = rec_new();
    dsd_iq_net_reader* r = reader_new(DSD_IQ_NET_KIND_TCP, port, DSD_IQ_NET_FORMAT_CS16, 48000U, rec);
    CHECK(holder != DSD_INVALID_SOCKET && r);
    dsd_iq_net_open_error err;
    CHECK(dsd_iq_net_reader_open(r, &err) == -1);
    CHECK(err.failure_kind == DSD_INPUT_FAILURE_REFUSED);
    CHECK(err.native_code != 0);
    char want[64];
    DSD_SNPRINTF(want, sizeof(want), "cannot connect to 127.0.0.1:%d (connection refused)", port);
    CHECK(strstr(err.message, want) != NULL);
    CHECK(dsd_iq_net_reader_state(r) == DSD_IQ_NET_STATE_STOPPED);
    CHECK(dsd_iq_net_reader_open(r, &err) == -1); /* once only */
    CHECK(dsd_iq_net_reader_start(r) == -1);
    dsd_iq_net_reader_run(r); /* returns at once */
    dsd_iq_net_reader_destroy(r);
    (void)dsd_socket_close(holder);
    rec_free(rec);
}

static void
test_reader_create_refusals(void) {
    dsd_iq_net_spec spec;
    CHECK(dsd_iq_net_spec_parse("iqtcp:127.0.0.1:1234:48k:cs16", &spec, NULL, 0) == 0);
    dsd_iq_net_spec bad = spec;
    bad.port = 0;
    CHECK(dsd_iq_net_reader_create(&bad, NULL) == NULL);
    bad = spec;
    bad.port = 65536;
    CHECK(dsd_iq_net_reader_create(&bad, NULL) == NULL);
    bad = spec;
    bad.format = DSD_IQ_NET_FORMAT_NONE;
    CHECK(dsd_iq_net_reader_create(&bad, NULL) == NULL);
    bad = spec;
    bad.host[0] = '\0';
    CHECK(dsd_iq_net_reader_create(&bad, NULL) == NULL);
    CHECK(dsd_iq_net_reader_create(NULL, NULL) == NULL);
    dsd_iq_net_reader* r = dsd_iq_net_reader_create(&spec, NULL);
    CHECK(r != NULL);
    CHECK(dsd_iq_net_reader_state(r) == DSD_IQ_NET_STATE_STOPPED);
    CHECK(dsd_iq_net_reader_local_port(r) == -1);
    dsd_iq_net_reader_note_written(r); /* only WAITING moves */
    CHECK(dsd_iq_net_reader_state(r) == DSD_IQ_NET_STATE_STOPPED);
    dsd_iq_net_reader_destroy(r);
    dsd_iq_net_reader_destroy(NULL);
    CHECK(dsd_iq_net_reader_available(NULL) == 0);
}

/* ---- UDP ------------------------------------------------------------------------------------------------------- */

typedef struct {
    dsd_socket_t sock;
    struct sockaddr_in to;
} udp_sender;

static int
udp_sender_open(udp_sender* s, int port) {
    int unused = 0;
    s->sock = bound_socket(SOCK_DGRAM, -1, &unused);
    loopback_addr(&s->to, port);
    return s->sock != DSD_INVALID_SOCKET;
}

static void
udp_send(const udp_sender* s, const uint8_t* p, size_t n) {
    const int k = dsd_socket_sendto(s->sock, p, n, 0, (const struct sockaddr*)&s->to, (int)sizeof(s->to));
    CHECK(k == (int)n);
}

static dsd_iq_net_reader*
udp_reader_open(dsd_iq_net_format format, uint32_t rate_hz, recorder* rec) {
    dsd_iq_net_reader* r = reader_new(DSD_IQ_NET_KIND_UDP, 0, format, rate_hz, rec);
    if (!open_ok(r)) {
        return r;
    }
    CHECK(dsd_iq_net_reader_local_port(r) > 0);
    CHECK(dsd_iq_net_reader_start(r) == 0);
    return r;
}

/* GNU Radio's default 1472-byte payload in each format, empty stop datagrams, and malformed ones. */
static void
test_udp_datagrams(void) {
    static const dsd_iq_net_format formats[] = {DSD_IQ_NET_FORMAT_CU8, DSD_IQ_NET_FORMAT_CS16, DSD_IQ_NET_FORMAT_CS32,
                                                DSD_IQ_NET_FORMAT_CF32};
    uint8_t payload[1472];
    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i * 7U + 3U);
    }
    for (size_t f = 0; f < sizeof(formats) / sizeof(formats[0]); f++) {
        recorder* rec = rec_new();
        dsd_iq_net_reader* r = udp_reader_open(formats[f], 48000U, rec);
        udp_sender a;
        CHECK(udp_sender_open(&a, dsd_iq_net_reader_local_port(r)));
        const size_t sb = dsd_iq_net_format_sample_bytes(formats[f]);
        const size_t per = sizeof(payload) / sb;
        udp_send(&a, payload, sizeof(payload));
        CHECK(wait_floats(rec, 2U * per));
        float want[2 * 736];
        (void)dsd_iq_net_convert(formats[f], payload, per, want);
        (void)dsd_mutex_lock(&rec->mu);
        for (size_t i = 0; i < 2U * per && i < rec->floats; i++) {
            CHECK(float_bits_equal(rec->iq[i], want[i]));
        }
        char peer[32];
        DSD_SNPRINTF(peer, sizeof(peer), "127.0.0.1:%d", local_port(a.sock));
        CHECK(rec->nevents == 1U && rec->events[0] == DSD_IQ_NET_EVENT_CONNECTED && strcmp(rec->peers[0], peer) == 0);
        (void)dsd_mutex_unlock(&rec->mu);
        CHECK(dsd_iq_net_reader_available(r) == 1);

        /* Three empty datagrams (a GNU Radio stop): ignored. */
        for (int i = 0; i < 3; i++) {
            udp_send(&a, payload, 0);
        }
        CHECK(WAIT_COUNTER(r, empty_datagrams, 3U));

        /* A datagram one byte short of whole samples is dropped whole and is a known loss; the next one aligns. */
        udp_send(&a, payload, sb + sb - 1U);
        CHECK(wait_event(rec, DSD_IQ_NET_EVENT_KNOWN_LOSS, 1U));
        udp_send(&a, payload, sb);
        CHECK(wait_floats(rec, 2U * per + 2U));
        (void)dsd_mutex_lock(&rec->mu);
        CHECK(rec->floats == 2U * per + 2U);
        CHECK(float_bits_equal(rec->iq[2U * per], want[0]) && float_bits_equal(rec->iq[2U * per + 1U], want[1]));
        CHECK(rec->lost_bytes[1] == 2U * sb - 1U);
        (void)dsd_mutex_unlock(&rec->mu);
        dsd_iq_net_counters c;
        dsd_iq_net_reader_get_counters(r, &c);
        CHECK(c.datagrams == 3U && c.empty_datagrams == 3U && c.malformed_datagrams == 1U && c.known_losses == 1U);
        CHECK(c.samples == per + 1U && c.bytes == sizeof(payload) + 3U * sb - 1U);
        CHECK(stop_ms(r) < k_prompt_ms / 3U);
        dsd_iq_net_reader_destroy(r);
        (void)dsd_socket_close(a.sock);
        rec_free(rec);
    }
    /* One warning per run naming the fix, however many runs there are: one per reader here. */
    CHECK(atomic_load(&g_log_malformed) == (int)(sizeof(formats) / sizeof(formats[0])));
}

/* One sender: the first valid one is locked, others are dropped and counted (one notice). A malformed datagram from
   a sender that is not locked locks nothing and loses nothing. After a stall the next valid sender is locked again:
   another sender is a reconnect, the same one a resume. Empty datagrams are no sign of life. */
static void
test_udp_one_sender_and_relock(void) {
    recorder* rec = rec_new();
    dsd_iq_net_reader* r = udp_reader_open(DSD_IQ_NET_FORMAT_CS16, 48000U, rec);
    const int port = dsd_iq_net_reader_local_port(r);
    udp_sender a, b;
    CHECK(udp_sender_open(&a, port));
    CHECK(udp_sender_open(&b, port));
    const uint8_t one[4] = {0x00, 0x40, 0x00, 0xC0};
    const uint8_t odd[3] = {1, 2, 3};

    udp_send(&b, odd, sizeof(odd)); /* malformed before any lock */
    CHECK(WAIT_COUNTER(r, malformed_datagrams, 1U));
    CHECK(rec_count_event(rec, DSD_IQ_NET_EVENT_KNOWN_LOSS) == 0U);
    CHECK(dsd_iq_net_reader_state(r) == DSD_IQ_NET_STATE_WAITING);

    udp_send(&a, one, sizeof(one));
    CHECK(wait_floats(rec, 2U));
    udp_send(&b, one, sizeof(one));
    udp_send(&b, one, sizeof(one));
    CHECK(WAIT_COUNTER(r, foreign_datagrams, 2U));
    CHECK(rec_floats(rec) == 2U);
    CHECK(atomic_load(&g_log_foreign) == 1);

    /* Empty datagrams from the locked sender do not hold the stall off. */
    advance_clock(1500U);
    udp_send(&a, one, 0);
    CHECK(WAIT_COUNTER(r, empty_datagrams, 1U));
    advance_clock(500U);
    CHECK(wait_event(rec, DSD_IQ_NET_EVENT_STALL, 1U));
    CHECK(dsd_iq_net_reader_available(r) == 0);

    /* B is locked now, with a reconnect before its first sample; A is the stranger. */
    udp_send(&b, one, sizeof(one));
    CHECK(wait_floats(rec, 4U));
    const int relock = rec_find_event(rec, DSD_IQ_NET_EVENT_RECONNECT, 0);
    CHECK(relock >= 0 && rec->event_at[relock] == 2U);
    char peer_b[32];
    DSD_SNPRINTF(peer_b, sizeof(peer_b), "127.0.0.1:%d", local_port(b.sock));
    CHECK(relock >= 0 && strcmp(rec->peers[relock], peer_b) == 0);
    udp_send(&a, one, sizeof(one));
    CHECK(WAIT_COUNTER(r, foreign_datagrams, 3U));
    CHECK(rec_floats(rec) == 4U);
    CHECK(atomic_load(&g_log_foreign) == 1);

    /* B stalls and comes back: a resume. */
    advance_clock(DSD_IQ_NET_STALL_MS);
    CHECK(wait_event(rec, DSD_IQ_NET_EVENT_STALL, 2U));
    udp_send(&b, one, sizeof(one));
    CHECK(wait_floats(rec, 6U));
    const int resume = rec_find_event(rec, DSD_IQ_NET_EVENT_RESUME, 0);
    CHECK(resume >= 0 && rec->event_at[resume] == 4U);
    CHECK(dsd_iq_net_reader_available(r) == 1);

    dsd_iq_net_counters c;
    dsd_iq_net_reader_get_counters(r, &c);
    CHECK(c.reconnects == 1U && c.resumes == 1U && c.stalls == 2U && c.known_losses == 0U);
    CHECK(rec_count_event(rec, DSD_IQ_NET_EVENT_CONNECTED) == 1U);
    dsd_iq_net_reader_destroy(r);
    (void)dsd_socket_close(a.sock);
    (void)dsd_socket_close(b.sock);
    rec_free(rec);
}

/* As test_tcp_stall_seen_after_the_wait(), over UDP: the late datagram ends a stall, seen before the sender is chosen,
   so the same sender is locked again with a resume. */
static void
test_udp_stall_seen_after_the_wait(void) {
    recorder* rec = rec_new();
    dsd_iq_net_reader* r = udp_reader_open(DSD_IQ_NET_FORMAT_CS16, 48000U, rec);
    const int port = dsd_iq_net_reader_local_port(r);
    udp_sender a;
    CHECK(udp_sender_open(&a, port));
    const uint8_t one[4] = {0x00, 0x40, 0x00, 0xC0};
    udp_send(&a, one, sizeof(one));
    CHECK(wait_floats(rec, 2U));
    dsd_sleep_ms(DSD_IQ_NET_WAIT_SLICE_MS / 4U);
    advance_clock(DSD_IQ_NET_STALL_MS + 1U);
    udp_send(&a, one, sizeof(one));
    CHECK(wait_floats(rec, 4U));
    CHECK(rec_count_event(rec, DSD_IQ_NET_EVENT_STALL) == 1U);
    const int resume = rec_find_event(rec, DSD_IQ_NET_EVENT_RESUME, 0);
    CHECK(resume >= 0 && rec->event_at[resume] == 2U);
    CHECK(rec_count_event(rec, DSD_IQ_NET_EVENT_RECONNECT) == 0U);
    dsd_iq_net_reader_destroy(r);
    (void)dsd_socket_close(a.sock);
    rec_free(rec);
}

/* The port is taken: open fails, and the reader stays stopped. */
static void
test_udp_bind_refused(void) {
    int port = 0;
    dsd_socket_t occupant = bound_socket(SOCK_DGRAM, -1, &port);
    recorder* rec = rec_new();
    dsd_iq_net_reader* r = reader_new(DSD_IQ_NET_KIND_UDP, port, DSD_IQ_NET_FORMAT_CS16, 48000U, rec);
    CHECK(occupant != DSD_INVALID_SOCKET && r);
    dsd_iq_net_open_error err;
    CHECK(dsd_iq_net_reader_open(r, &err) == -1);
    CHECK(err.native_code != 0);
    char want[64];
    DSD_SNPRINTF(want, sizeof(want), "cannot receive on 127.0.0.1:%d", port);
    CHECK(strstr(err.message, want) != NULL);
    CHECK(dsd_iq_net_reader_state(r) == DSD_IQ_NET_STATE_STOPPED);
    dsd_iq_net_reader_destroy(r);
    (void)dsd_socket_close(occupant);
    rec_free(rec);
}

/* Through the reader: 15 % fewer samples than the rate for the warm-up and three windows warns once. */
static void
test_udp_rate_warning(void) {
    const int warnings_before = atomic_load(&g_log_rate);
    recorder* rec = rec_new();
    dsd_iq_net_reader* r = udp_reader_open(DSD_IQ_NET_FORMAT_CU8, 8000U, rec);
    udp_sender a;
    CHECK(udp_sender_open(&a, dsd_iq_net_reader_local_port(r)));
    /* 6800 samples/s, in 1 s steps of two 3400-sample (6800-byte) datagrams' worth, sent as 1360-byte datagrams. */
    uint8_t chunk[1360];
    DSD_MEMSET(chunk, 0x80, sizeof(chunk));
    size_t floats = 0;
    const uint64_t steps = (IQ_NET_RATE_WARMUP_MS + 3U * IQ_NET_RATE_WINDOW_MS) / 1000U + 1U;
    for (uint64_t s = 0; s < steps; s++) {
        for (int k = 0; k < 10; k++) {
            udp_send(&a, chunk, sizeof(chunk));
        }
        floats += 10U * sizeof(chunk);
        if (!wait_floats(rec, floats)) {
            break;
        }
        advance_clock(1000U);
    }
    CHECK(WAIT_COUNTER(r, rate_warnings, 1U));
    CHECK(rec_count_event(rec, DSD_IQ_NET_EVENT_STALL) == 0U);
    dsd_iq_net_reader_destroy(r);
    CHECK(atomic_load(&g_log_rate) == warnings_before + 1);
    (void)dsd_socket_close(a.sock);
    rec_free(rec);
}

int
main(void) {
    if (dsd_socket_init() != 0) {
        DSD_FPRINTF(stderr, "socket init failed\n");
        return 1;
    }
    atomic_init(&g_log_malformed, 0);
    atomic_init(&g_log_foreign, 0);
    atomic_init(&g_log_rate, 0);
    dsd_atomic_u64_init(&g_clock_ms, 100000U);
    dsd_neo_log_set_tap(log_tap, NULL);

    test_split_fields();
    test_spec_parse_ok();
    test_spec_parse_refusals();
    test_spec_format_refusals();
    test_format_table();
    test_convert_cu8();
    test_convert_cs8();
    test_convert_cs16();
    test_convert_cs32();
    test_convert_cf32_bounded();
    test_carry_every_split();
    test_rate_rows();
    test_reader_create_refusals();
    test_tcp_one_byte_writes();
    test_tcp_carry_discarded_on_reconnect();
    test_tcp_stall_vs_close();
    test_tcp_stall_seen_after_the_wait();
    test_tcp_backoff_cancellable();
    test_tcp_stop_during_recv();
    test_tcp_stop_during_connect();
    test_tcp_connect_refused();
    test_udp_datagrams();
    test_udp_one_sender_and_relock();
    test_udp_stall_seen_after_the_wait();
    test_udp_bind_refused();
    test_udp_rate_warning();

    dsd_socket_cleanup();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "IO_IQ_NET: %d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "IO_IQ_NET: all passed\n");
    return 0;
}
