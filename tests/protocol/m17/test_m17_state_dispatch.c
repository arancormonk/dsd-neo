// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// or invalid-value negative vectors to exercise guarded behavior.
// NOLINTBEGIN(bugprone-implicit-widening-of-multiplication-result)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "fixtures/m17_reference_vectors.h"

#include <assert.h>
#include <dsd-neo/core/audio_filters.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/analog_rx.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/protocol/m17/m17.h>
#include <dsd-neo/runtime/control_pump.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/m17_udp_hooks.h>
#include <dsd-neo/runtime/net_audio_input_hooks.h>
#ifdef USE_RADIO
#include <dsd-neo/runtime/rtl_stream_io_hooks.h>
#endif
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <math.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/dibit.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/dsp/analog_voice.h"
#include "dsd-neo/platform/audio.h"
#include "dsd-neo/platform/sockets.h"
#include "dsd-neo/protocol/m17/m17_parse.h"
#include "dsd-neo/protocol/m17/m17_tables.h"
#include "m17_algorithms.h"
#include "m17_confirm.h"
#include "m17_internal.h"
#include "test_support.h"

struct CODEC2;

static dsd_opts g_opts;
static dsd_state g_state;
#ifdef USE_CODEC2
static int g_codec2_decode_calls;
static unsigned char g_codec2_last_bits[8];
static int g_udp_audio_calls;
static size_t g_udp_audio_last_nsam;
static const dsd_opts* g_udp_audio_last_opts;
static dsd_state* g_udp_audio_last_state;
static short g_udp_audio_last_first_sample;
#endif
static int g_conv_start_calls;
static int g_conv_decode_calls;
static int g_conv_chainback_calls;
static unsigned int g_conv_chainback_bits;
static int g_m17_connect_calls;
static int g_m17_connect_result;
static int g_m17_receiver_calls;
static uint8_t g_conv_first_symbols[8];

enum { TEST_M17_LSF_BITS = M17_LSF_BYTES * 8U };

// NOLINTNEXTLINE(misc-use-internal-linkage)
void CNXDNConvolution_start(void);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void CNXDNConvolution_decode(uint8_t s0, uint8_t s1);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void CNXDNConvolution_chainback(unsigned char* out, unsigned int nBits);
#ifdef USE_CODEC2
// NOLINTNEXTLINE(misc-use-internal-linkage)
void codec2_decode(struct CODEC2* codec2_state, short speech[], const unsigned char* bits);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void codec2_encode(struct CODEC2* codec2_state, unsigned char* bits, short speech[]);
// NOLINTNEXTLINE(misc-use-internal-linkage)
int codec2_samples_per_frame(struct CODEC2* codec2_state);
#endif
// NOLINTNEXTLINE(misc-use-internal-linkage)
void LFSRN(const char* BufferIn, char* BufferOut, dsd_state* state);
// NOLINTNEXTLINE(misc-use-internal-linkage)
int Connect(char* hostname, int portno);

void
CNXDNConvolution_start(void) {
    g_conv_start_calls++;
}

void
CNXDNConvolution_decode(uint8_t s0, uint8_t s1) {
    const int index = g_conv_decode_calls * 2;
    if (index + 1 < (int)sizeof(g_conv_first_symbols)) {
        g_conv_first_symbols[index] = s0;
        g_conv_first_symbols[index + 1] = s1;
    }
    g_conv_decode_calls++;
}

void
CNXDNConvolution_chainback(unsigned char* out, unsigned int nBits) {
    g_conv_chainback_calls++;
    g_conv_chainback_bits = nBits;
    if (out != NULL) {
        const unsigned int byte_count = (nBits + 7U) / 8U;
        for (unsigned int i = 0U; i < byte_count; i++) {
            out[i] = (unsigned char)(0xA0U + i);
        }
    }
}

#ifdef USE_CODEC2
void
codec2_decode(struct CODEC2* codec2_state, short speech[], const unsigned char* bits) {
    (void)codec2_state;
    g_codec2_decode_calls++;
    if (bits != NULL) {
        DSD_MEMCPY(g_codec2_last_bits, bits, sizeof(g_codec2_last_bits));
    }
    if (speech != NULL) {
        for (size_t i = 0U; i < 160U; i++) {
            speech[i] = (short)(1000 + g_codec2_decode_calls + (int)i);
        }
    }
}

/* The peak of each 160-sample frame the stream encoder hands codec2, in order (issue #625: closed samples reach it as
   silence). */
static int g_codec2_encode_count = 0;
static int g_codec2_encode_peaks[256];

void
codec2_encode(struct CODEC2* codec2_state, unsigned char* bits, short speech[]) {
    (void)codec2_state;
    (void)bits;
    int peak = 0;
    for (size_t i = 0U; speech != NULL && i < 160U; i++) {
        const int v = speech[i] < 0 ? -(int)speech[i] : (int)speech[i];
        peak = v > peak ? v : peak;
    }
    if (g_codec2_encode_count < (int)(sizeof(g_codec2_encode_peaks) / sizeof(g_codec2_encode_peaks[0]))) {
        g_codec2_encode_peaks[g_codec2_encode_count] = peak;
    }
    g_codec2_encode_count++;
}

int
codec2_samples_per_frame(struct CODEC2* codec2_state) {
    (void)codec2_state;
    return 160;
}
#endif

void
LFSRN(const char* BufferIn, char* BufferOut, dsd_state* state) {
    (void)BufferIn;
    (void)BufferOut;
    (void)state;
}

int
Connect(char* hostname, int portno) {
    (void)hostname;
    (void)portno;
    return -1;
}

static int
fake_m17_connect_failure(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_m17_connect_calls++;
    return g_m17_connect_result;
}

static dsd_socket_t
fake_m17_bind_failure(char* hostname, int portno) {
    (void)hostname;
    (void)portno;
    return DSD_INVALID_SOCKET;
}

static dsd_socket_t
fake_m17_bind_success(char* hostname, int portno) {
    (void)hostname;
    (void)portno;
    return (dsd_socket_t)17;
}

static int
fake_m17_receiver_failure(const dsd_opts* opts, void* data) {
    (void)opts;
    (void)data;
    g_m17_receiver_calls++;
    return -1;
}

static int
fake_m17_receiver_idle_then_shutdown(const dsd_opts* opts, void* data) {
    (void)opts;
    (void)data;
    g_m17_receiver_calls++;
    if (g_m17_receiver_calls == 2) {
        exitflag = 1;
    }
    return 0;
}

#ifdef USE_CODEC2
static void
fake_udp_audio_blast(const dsd_opts* opts, dsd_state* state, size_t nsam, const void* data) {
    g_udp_audio_calls++;
    g_udp_audio_last_opts = opts;
    g_udp_audio_last_state = state;
    g_udp_audio_last_nsam = nsam;
    g_udp_audio_last_first_sample = (data != NULL) ? ((const short*)data)[0] : 0;
}

static void
reset_audio_fakes(void) {
    dsd_udp_audio_hooks hooks = {0};
    dsd_udp_audio_hooks_set(hooks);
    g_codec2_decode_calls = 0;
    DSD_MEMSET(g_codec2_last_bits, 0, sizeof(g_codec2_last_bits));
    g_udp_audio_calls = 0;
    g_udp_audio_last_nsam = 0U;
    g_udp_audio_last_opts = NULL;
    g_udp_audio_last_state = NULL;
    g_udp_audio_last_first_sample = 0;
}
#endif

static void
reset_convolution_fake(void) {
    g_conv_start_calls = 0;
    g_conv_decode_calls = 0;
    g_conv_chainback_calls = 0;
    g_conv_chainback_bits = 0U;
    DSD_MEMSET(g_conv_first_symbols, 0, sizeof(g_conv_first_symbols));
}

#ifdef USE_CODEC2
static void
install_fake_udp_audio(void) {
    dsd_udp_audio_hooks hooks = {0};
    hooks.blast = fake_udp_audio_blast;
    dsd_udp_audio_hooks_set(hooks);
}
#endif

static void
bytes_to_bits(const uint8_t* bytes, uint8_t* bits, size_t byte_count) {
    for (size_t byte = 0U; byte < byte_count; byte++) {
        for (size_t bit = 0U; bit < 8U; bit++) {
            bits[(byte * 8U) + bit] = (uint8_t)((bytes[byte] >> (7U - bit)) & 1U);
        }
    }
}

static int
expect_u64(const char* label, unsigned long long got, unsigned long long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got 0x%llX want 0x%llX\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
get_m17_call(const dsd_state* state, dsd_call_snapshot* call) {
    DSD_MEMSET(call, 0, sizeof(*call));
    return dsd_call_state_get(state, 0U, call);
}

static int
expect_u8(const char* label, uint8_t got, uint8_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %u want %u\n", label, (unsigned)got, (unsigned)want);
        return 1;
    }
    return 0;
}

static int
expect_int(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_bytes(const char* label, const uint8_t* got, const uint8_t* want, size_t n) {
    if (memcmp(got, want, n) != 0) {
        DSD_FPRINTF(stderr, "%s: byte mismatch\n", label);
        return 1;
    }
    return 0;
}

static unsigned long long
bits_to_u64(const uint8_t* bits, size_t n) {
    unsigned long long value = 0ULL;
    for (size_t i = 0U; i < n; i++) {
        value = (value << 1U) | (unsigned long long)(bits[i] & 1U);
    }
    return value;
}

static void
write_bits_from_u64(uint8_t* bits, unsigned long long value, size_t n) {
    for (size_t i = 0U; i < n; i++) {
        const size_t shift = n - 1U - i;
        bits[i] = (uint8_t)((value >> shift) & 1U);
    }
}

static void
pack_bits_msb(const uint8_t* bits, size_t bit_count, uint8_t* bytes, size_t byte_count) {
    DSD_MEMSET(bytes, 0, byte_count);
    for (size_t bit = 0U; bit < bit_count; bit++) {
        if ((bits[bit] & 1U) != 0U) {
            bytes[bit / 8U] |= (uint8_t)(1U << (7U - (bit % 8U)));
        }
    }
}

static void
expect_signature_slice(uint8_t* out, size_t offset) {
    bytes_to_bits(M17_REF_SIGNATURE_BYTES + offset, out, 16U);
}

static void
build_lsf_bits(uint8_t lsf_bits[TEST_M17_LSF_BITS], unsigned long long dst, unsigned long long src, uint16_t type_word,
               const uint8_t meta[M17_AES_NONCE_BYTES]) {
    DSD_MEMSET(lsf_bits, 0, TEST_M17_LSF_BITS);
    m17_load_lsf_callsigns(lsf_bits, dst, src);
    write_bits_from_u64(lsf_bits + 96, type_word, 16U);
    if (meta != NULL) {
        for (size_t i = 0U; i < M17_AES_NONCE_BYTES; i++) {
            write_bits_from_u64(lsf_bits + 112U + (i * 8U), meta[i], 8U);
        }
    }
}

static void
build_encoded_lich_chunk(const uint8_t lsf_bits[TEST_M17_LSF_BITS], uint8_t lich_counter,
                         uint8_t encoded[M17_LICH_BITS]) {
    uint8_t content[M17_LICH_CONTENT_BITS];
    DSD_MEMSET(content, 0, sizeof(content));
    DSD_MEMSET(encoded, 0, M17_LICH_BITS);
    assert(m17_lich_build_content(lsf_bits, lich_counter, content) == 0);
    m17_lich_encode_bits(content, encoded);
}

static void
build_ip_stream_frame(uint8_t ip_frame[54], const uint8_t lsf_bits[TEST_M17_LSF_BITS], uint16_t sid, uint16_t fn,
                      uint8_t eot, const uint8_t payload_bits[M17_STREAM_PAYLOAD_BITS]) {
    uint8_t ip_bits[52U * 8U];
    DSD_MEMSET(ip_bits, 0, sizeof(ip_bits));

    write_bits_from_u64(ip_bits, (unsigned long long)'M', 8U);
    write_bits_from_u64(ip_bits + 8, (unsigned long long)'1', 8U);
    write_bits_from_u64(ip_bits + 16, (unsigned long long)'7', 8U);
    write_bits_from_u64(ip_bits + 24, (unsigned long long)' ', 8U);
    write_bits_from_u64(ip_bits + 32, sid, 16U);
    for (size_t i = 0U; i < M17_LSF_LSD_BITS; i++) {
        ip_bits[48U + i] = lsf_bits[i];
    }
    ip_bits[272] = (uint8_t)(eot & 1U);
    write_bits_from_u64(ip_bits + 273, fn & 0x7FFFU, 15U);
    for (size_t i = 0U; i < M17_STREAM_PAYLOAD_BITS; i++) {
        ip_bits[288U + i] = payload_bits[i];
    }

    pack_bits_msb(ip_bits, sizeof(ip_bits), ip_frame, 52U);
    const uint16_t crc = m17_crc16(ip_frame, 52U);
    ip_frame[52] = (uint8_t)((crc >> 8U) & 0xFFU);
    ip_frame[53] = (uint8_t)(crc & 0xFFU);
}

static size_t
build_ip_mpkt_frame(uint8_t* ip_frame, size_t ip_frame_len, const uint8_t lsf_bits[TEST_M17_LSF_BITS], uint16_t sid,
                    const uint8_t* app, size_t app_len) {
    const size_t frame_len = 34U + app_len + 3U;
    if (ip_frame == NULL || lsf_bits == NULL || app == NULL || ip_frame_len < frame_len) {
        return 0U;
    }

    DSD_MEMSET(ip_frame, 0, ip_frame_len);
    ip_frame[0] = 'M';
    ip_frame[1] = 'P';
    ip_frame[2] = 'K';
    ip_frame[3] = 'T';
    ip_frame[4] = (uint8_t)((sid >> 8U) & 0xFFU);
    ip_frame[5] = (uint8_t)(sid & 0xFFU);
    pack_bits_msb(lsf_bits, M17_LSF_LSD_BITS, ip_frame + 6, M17_LSF_LSD_BYTES);
    DSD_MEMCPY(ip_frame + 34, app, app_len);

    const uint16_t crc = m17_crc16(ip_frame, (uint16_t)(frame_len - 2U));
    ip_frame[frame_len - 2U] = (uint8_t)((crc >> 8U) & 0xFFU);
    ip_frame[frame_len - 1U] = (uint8_t)(crc & 0xFFU);
    return frame_len;
}

static struct m17_lsf_result
valid_lsf_result(void) {
    struct m17_lsf_result res;
    DSD_MEMSET(&res, 0, sizeof(res));
    res.dst = 0x0000009FDD51ULL;
    res.src = 0x0000009FDD51ULL;
    res.type_word = 0x0555U;
    res.packet_stream = 1U;
    res.dt = 2U;
    res.et = 2U;
    res.es = 0U;
    res.cn = 9U;
    res.signature = 1U;
    res.meta_is_iv = 1U;
    res.dst_is_valid = 1U;
    res.src_is_valid = 1U;
    res.type_reserved_valid = 1U;
    DSD_MEMCPY(res.dst_csd, "AB1CD", 6U);
    DSD_MEMCPY(res.src_csd, "AB1CD", 6U);
    DSD_MEMCPY(res.meta, M17_REF_AES_NONCE, sizeof(res.meta));
    return res;
}

static int
test_embedded_lich_chunks_store_and_finalize_lsf_state(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t lsf_packed[M17_LSF_BYTES];
    uint8_t encoded[M17_LICH_BITS];

    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMSET(lsf_packed, 0, sizeof(lsf_packed));

    const unsigned long long dst = m17_encode_b40_callsign(0ULL, M17_REF_LSF_DST_CSD);
    const unsigned long long src = m17_encode_b40_callsign(0ULL, M17_REF_LSF_SRC_CSD);
    const uint16_t type_word = m17_compose_frame_info(1U, 2U, 0U, 0U, 6U, 1U, 0U);
    build_lsf_bits(lsf_bits, dst, src, type_word, NULL);
    (void)m17_attach_lsf_crc(lsf_bits, lsf_packed);

    int err = 0;
    for (uint8_t chunk = 0U; chunk < (M17_LICH_CHUNKS - 1U); chunk++) {
        build_encoded_lich_chunk(lsf_bits, chunk, encoded);
        err |= expect_int("LICH chunk accepted", m17_process_lich(state, opts, encoded), 0);
        for (size_t bit = 0U; bit < M17_LICH_CHUNK_BITS; bit++) {
            err |= expect_u8("LICH chunk stored", state->m17_lsf[((size_t)chunk * M17_LICH_CHUNK_BITS) + bit],
                             lsf_bits[((size_t)chunk * M17_LICH_CHUNK_BITS) + bit]);
        }
        dsd_call_snapshot call;
        err |= expect_int("LICH does not decode before final chunk", get_m17_call(state, &call), 0);
    }

    build_encoded_lich_chunk(lsf_bits, (uint8_t)(M17_LICH_CHUNKS - 1U), encoded);
    err |= expect_int("final LICH chunk accepted", m17_process_lich(state, opts, encoded), 0);
    dsd_call_snapshot call;
    err |= expect_int("final LICH publishes call", get_m17_call(state, &call), 1);
    err |= expect_u64("final LICH decodes dst", call.ota_target_id, dst);
    err |= expect_u64("final LICH decodes src", call.ota_source_id, src);
    err |= expect_u8("final LICH decodes data type", state->m17_str_dt, 2U);
    err |= expect_u8("final LICH decodes CAN", state->m17_can, 6U);
    err |= expect_u8("final LICH clear encryption", state->m17_enc, 0U);
    err |= expect_u8("final LICH advertises signature", state->m17_signature_advertised, 1U);
    for (size_t bit = 0U; bit < TEST_M17_LSF_BITS; bit++) {
        err |= expect_u8("final LICH clears staged LSF", state->m17_lsf[bit], 0U);
    }
    if (strcmp(call.target_text, M17_REF_LSF_DST_CSD) != 0 || strcmp(call.source_text, M17_REF_LSF_SRC_CSD) != 0) {
        DSD_FPRINTF(stderr, "final LICH callsigns: dst='%s' src='%s'\n", call.target_text, call.source_text);
        err |= 1;
    }
    dsd_state_ext_free_all(state);
    return err;
}

static int
test_embedded_lich_rejects_invalid_counter_and_gates_bad_lsf_crc(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t lsf_packed[M17_LSF_BYTES];
    uint8_t encoded[M17_LICH_BITS];

    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMSET(lsf_packed, 0, sizeof(lsf_packed));

    const unsigned long long dst = m17_encode_b40_callsign(0ULL, M17_REF_LSF_DST_CSD);
    const unsigned long long src = m17_encode_b40_callsign(0ULL, M17_REF_LSF_SRC_CSD);
    const uint16_t type_word = m17_compose_frame_info(1U, 2U, 0U, 0U, 7U, 0U, 0U);
    build_lsf_bits(lsf_bits, dst, src, type_word, NULL);
    (void)m17_attach_lsf_crc(lsf_bits, lsf_packed);

    uint8_t invalid_content[M17_LICH_CONTENT_BITS];
    DSD_MEMSET(invalid_content, 0, sizeof(invalid_content));
    invalid_content[40] = 1U;
    invalid_content[41] = 1U;
    invalid_content[42] = 0U;
    m17_lich_encode_bits(invalid_content, encoded);
    DSD_MEMSET(state->m17_lsf, 0x5AU, sizeof(state->m17_lsf));

    int err = 0;
    err |= expect_int("invalid-counter LICH chunk rejected", m17_process_lich(state, opts, encoded), -1);
    for (size_t bit = 0U; bit < TEST_M17_LSF_BITS; bit++) {
        err |= expect_u8("invalid-counter LICH preserves staged LSF", state->m17_lsf[bit], 0x5AU);
    }

    DSD_MEMSET(state, 0, sizeof(*state));
    opts->aggressive_framesync = 1;
    lsf_bits[M17_LSF_LSD_BITS + 3U] ^= 1U;
    for (uint8_t chunk = 0U; chunk < M17_LICH_CHUNKS; chunk++) {
        build_encoded_lich_chunk(lsf_bits, chunk, encoded);
        err |= expect_int("bad CRC LICH chunk accepted", m17_process_lich(state, opts, encoded), 0);
    }
    dsd_call_snapshot call;
    err |= expect_int("bad LSF CRC does not publish call", get_m17_call(state, &call), 0);
    for (size_t bit = 0U; bit < TEST_M17_LSF_BITS; bit++) {
        err |= expect_u8("bad LSF CRC clears staged LSF", state->m17_lsf[bit], 0U);
    }

    DSD_MEMSET(state, 0, sizeof(*state));
    opts->aggressive_framesync = 0;
    for (uint8_t chunk = 0U; chunk < M17_LICH_CHUNKS; chunk++) {
        build_encoded_lich_chunk(lsf_bits, chunk, encoded);
        err |= expect_int("relaxed bad CRC LICH chunk accepted", m17_process_lich(state, opts, encoded), 0);
    }
    err |= expect_int("relaxed bad LSF CRC does not publish call", get_m17_call(state, &call), 0);
    err |= expect_u8("relaxed bad LSF CRC decodes data type", state->m17_str_dt, 2U);
    err |= expect_u8("relaxed bad LSF CRC decodes CAN", state->m17_can, 7U);
    err |= expect_u8("relaxed bad LSF CRC decodes encryption", state->m17_enc, 0U);
    return err;
}

static int
test_rf_lsf_crc_policy_preserves_relaxed_decode(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t lsf_packed[M17_LSF_BYTES];

    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    const unsigned long long dst = m17_encode_b40_callsign(0ULL, M17_REF_LSF_DST_CSD);
    const unsigned long long src = m17_encode_b40_callsign(0ULL, M17_REF_LSF_SRC_CSD);
    const uint16_t type_word = m17_compose_frame_info(1U, 2U, 1U, 2U, 5U, 0U, 0U);
    build_lsf_bits(lsf_bits, dst, src, type_word, NULL);
    const uint16_t crc = m17_attach_lsf_crc(lsf_bits, lsf_packed);

    opts->aggressive_framesync = 1;
    DSD_MEMCPY(state->m17_lsf, lsf_bits, sizeof(lsf_bits));
    state->m17_str_dt = 3U;
    state->m17_can = 9U;
    int err = 0;
    err |= expect_int("strict RF LSF reports CRC error", m17_finalize_lsf_crc(opts, state, lsf_packed, crc ^ 1U), 1);
    err |= expect_u8("strict RF LSF preserves data type", state->m17_str_dt, 3U);
    err |= expect_u8("strict RF LSF preserves CAN", state->m17_can, 9U);

    opts->aggressive_framesync = 0;
    DSD_MEMCPY(state->m17_lsf, lsf_bits, sizeof(lsf_bits));
    err |= expect_int("relaxed RF LSF reports CRC error", m17_finalize_lsf_crc(opts, state, lsf_packed, crc ^ 1U), 1);
    err |= expect_u8("relaxed RF LSF decodes data type", state->m17_str_dt, 2U);
    err |= expect_u8("relaxed RF LSF decodes CAN", state->m17_can, 5U);
    err |= expect_u8("relaxed RF LSF decodes encryption", state->m17_enc, 1U);
    dsd_call_snapshot call;
    err |= expect_int("relaxed RF LSF CRC does not publish call", get_m17_call(state, &call), 0);
    return err;
}

static int
publish_m17_voice_lsf(dsd_opts* opts, dsd_state* state, uint8_t encryption_type, uint8_t encryption_subtype) {
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t lsf_packed[M17_LSF_BYTES];
    const unsigned long long dst = m17_encode_b40_callsign(0ULL, M17_REF_LSF_DST_CSD);
    const unsigned long long src = m17_encode_b40_callsign(0ULL, M17_REF_LSF_SRC_CSD);
    const uint16_t type_word = m17_compose_frame_info(1U, 2U, encryption_type, encryption_subtype, 5U, 0U, 0U);
    build_lsf_bits(lsf_bits, dst, src, type_word, M17_REF_AES_NONCE);
    const uint16_t crc = m17_attach_lsf_crc(lsf_bits, lsf_packed);
    DSD_MEMCPY(state->m17_lsf, lsf_bits, sizeof(lsf_bits));
    return m17_finalize_lsf_crc(opts, state, lsf_packed, crc);
}

static int
test_lsf_crypto_availability_matches_payload_validation(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    dsd_call_snapshot call;
    int err = 0;

    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    state->aes_key_loaded[0] = 1;
    state->aes_key_segments[0] = 2U;
    DSD_MEMCPY(state->aes_key, M17_REF_AES128_KEY, sizeof(M17_REF_AES128_KEY));
    err |= expect_int("AES-128 LSF accepted", publish_m17_voice_lsf(opts, state, 2U, 0U), 0);
    err |= expect_int("AES-128 LSF publishes call", get_m17_call(state, &call), 1);
    err |= expect_int("AES-128 LSF decryptable", call.crypto, DSD_CALL_CRYPTO_DECRYPTABLE);
    err |= expect_u8("AES-128 LSF permits audio", call.audio_permitted, 1U);

    dsd_state_ext_free_all(state);
    DSD_MEMSET(state, 0, sizeof(*state));
    state->aes_key_loaded[0] = 1;
    state->aes_key_segments[0] = 2U;
    DSD_MEMCPY(state->aes_key, M17_REF_AES128_KEY, sizeof(M17_REF_AES128_KEY));
    err |= expect_int("short AES-256 LSF accepted", publish_m17_voice_lsf(opts, state, 2U, 2U), 0);
    err |= expect_int("short AES-256 LSF publishes call", get_m17_call(state, &call), 1);
    err |= expect_int("short AES-256 LSF remains pending", call.crypto, DSD_CALL_CRYPTO_ENCRYPTED_PENDING);
    err |= expect_u8("short AES-256 LSF blocks audio", call.audio_permitted, 0U);

    dsd_state_ext_free_all(state);
    DSD_MEMSET(state, 0, sizeof(*state));
    state->R = 0x100U;
    err |= expect_int("masked-zero scrambler LSF accepted", publish_m17_voice_lsf(opts, state, 1U, 0U), 0);
    err |= expect_int("masked-zero scrambler LSF publishes call", get_m17_call(state, &call), 1);
    err |= expect_int("masked-zero scrambler remains pending", call.crypto, DSD_CALL_CRYPTO_ENCRYPTED_PENDING);
    err |= expect_u8("masked-zero scrambler blocks audio", call.audio_permitted, 0U);

    dsd_state_ext_free_all(state);
    return err;
}

static int
test_lsf_application_resets_and_stores_state(void) {
    dsd_state* state = &g_state;
    DSD_MEMSET(state, 0, sizeof(*state));
    state->m17_can = 3U;
    state->m17_enc = 1U;
    state->m17_payload_decrypted = 1U;
    state->m17_signature_received_mask = 0x0FU;
    state->m17_signature_complete = 1U;
    state->m17_signature_bad_sequence = 1U;
    state->m17_signature_verification_status = 99U;
    DSD_MEMSET(state->m17_signature_digest, 0xA5, sizeof(state->m17_signature_digest));
    DSD_MEMSET(state->m17_signature, 0x5A, sizeof(state->m17_signature));

    struct m17_lsf_result res = valid_lsf_result();
    int err = 0;
    err |= expect_int("valid LSF applied", m17_apply_lsf_result(state, &res), 1);
    err |= expect_u8("LSF CAN", state->m17_can, res.cn);
    err |= expect_u8("LSF dt", state->m17_str_dt, res.dt);
    err |= expect_u8("LSF enc", state->m17_enc, res.et);
    err |= expect_u8("LSF enc subtype", state->m17_enc_st, res.es);
    err |= expect_u8("LSF payload decrypted reset", state->m17_payload_decrypted, 0U);
    err |= expect_u8("LSF signature advertised", state->m17_signature_advertised, 1U);
    err |= expect_u8("LSF signature mask reset", state->m17_signature_received_mask, 0U);
    err |= expect_u8("LSF signature complete reset", state->m17_signature_complete, 0U);
    err |= expect_u8("LSF signature sequence reset", state->m17_signature_bad_sequence, 0U);
    err |= expect_u8("LSF signature status reset", state->m17_signature_verification_status, 0U);
    err |= expect_bytes("LSF meta nonce", state->m17_meta, M17_REF_AES_NONCE, sizeof(M17_REF_AES_NONCE));
    for (size_t i = 0U; i < sizeof(state->m17_signature_digest); i++) {
        err |= expect_u8("LSF digest reset", state->m17_signature_digest[i], 0U);
    }
    for (size_t i = 0U; i < sizeof(state->m17_signature); i++) {
        err |= expect_u8("LSF signature buffer reset", state->m17_signature[i], 0U);
    }
    return err;
}

static int
test_lsf_rejects_reserved_type_without_replacing_state(void) {
    dsd_state* state = &g_state;
    DSD_MEMSET(state, 0, sizeof(*state));
    state->m17_can = 4U;
    state->m17_enc = 1U;

    struct m17_lsf_result res = valid_lsf_result();
    res.rs = 1U;

    int err = 0;
    err |= expect_int("reserved LSF rejected", m17_apply_lsf_result(state, &res), 0);
    err |= expect_u8("reserved LSF CAN preserved", state->m17_can, 4U);
    err |= expect_u8("reserved LSF enc preserved", state->m17_enc, 1U);
    return err;
}

static int
test_lsf_rejects_invalid_addresses_without_replacing_state(void) {
    dsd_state* state = &g_state;
    DSD_MEMSET(state, 0, sizeof(*state));
    state->m17_can = 6U;
    state->m17_str_dt = 3U;

    struct m17_lsf_result res = valid_lsf_result();
    res.dst_is_valid = 0U;

    int err = 0;
    err |= expect_int("invalid destination rejected", m17_apply_lsf_result(state, &res), 0);
    err |= expect_u8("invalid destination preserves CAN", state->m17_can, 6U);
    err |= expect_u8("invalid destination preserves data type", state->m17_str_dt, 3U);

    res = valid_lsf_result();
    res.src_is_valid = 0U;
    err |= expect_int("invalid source rejected", m17_apply_lsf_result(state, &res), 0);
    err |= expect_u8("invalid source preserves CAN", state->m17_can, 6U);
    err |= expect_u8("invalid source preserves data type", state->m17_str_dt, 3U);
    return err;
}

static int
test_lsf_null_meta_decodes_text_and_honors_can_filter(void) {
    dsd_state* state = &g_state;
    uint8_t expected_text[M17_TEXT_BLOCK_BYTES];
    DSD_MEMSET(expected_text, 0, sizeof(expected_text));
    DSD_MEMCPY(expected_text, "LSF-META-TEXT", M17_TEXT_BLOCK_BYTES);

    struct m17_lsf_result res = valid_lsf_result();
    res.et = 0U;
    res.es = 0U;
    res.has_meta = 1U;
    res.meta_is_iv = 0U;
    res.cn = 9U;
    DSD_MEMSET(res.meta, 0, sizeof(res.meta));
    res.meta[0] = 0x11U;
    DSD_MEMCPY(res.meta + 1, expected_text, M17_TEXT_BLOCK_BYTES);

    DSD_MEMSET(state, 0, sizeof(*state));
    state->m17_can_en = -1;

    int err = 0;
    err |= expect_int("null META LSF accepted", m17_apply_lsf_result(state, &res), 1);
    err |= expect_u8("null META text expected bitmap", state->m17_text_meta_expected_bitmap, 0x01U);
    err |= expect_u8("null META text received bitmap", state->m17_text_meta_received_bitmap, 0x01U);
    err |= expect_u8("null META text control", state->m17_text_meta_control_or, 0x11U);
    err |= expect_bytes("null META text bytes", state->m17_text_meta, expected_text, sizeof(expected_text));
    err |= expect_bytes("null META payload stored", state->m17_meta, res.meta, sizeof(res.meta));

    DSD_MEMSET(state, 0, sizeof(*state));
    state->m17_can_en = 4;
    state->m17_text_meta_expected_bitmap = 0x0FU;
    state->m17_text_meta_received_bitmap = 0x02U;
    state->m17_text_meta_control_or = 0xF2U;
    DSD_MEMSET(state->m17_text_meta, 0x5AU, sizeof(state->m17_text_meta));

    err |= expect_int("CAN-filtered null META LSF accepted", m17_apply_lsf_result(state, &res), 1);
    err |= expect_u8("CAN-filtered null META preserves expected", state->m17_text_meta_expected_bitmap, 0x0FU);
    err |= expect_u8("CAN-filtered null META preserves received", state->m17_text_meta_received_bitmap, 0x02U);
    err |= expect_u8("CAN-filtered null META preserves control", state->m17_text_meta_control_or, 0xF2U);
    for (size_t i = 0U; i < sizeof(state->m17_text_meta); i++) {
        err |= expect_u8("CAN-filtered null META preserves text", state->m17_text_meta[i], 0x5AU);
    }

    DSD_MEMSET(state, 0, sizeof(*state));
    state->m17_can_en = -1;
    state->m17_text_meta_expected_bitmap = 0x0FU;
    state->m17_text_meta_received_bitmap = 0x02U;
    state->m17_text_meta_control_or = 0xF2U;
    DSD_MEMSET(state->m17_text_meta, 0x5AU, sizeof(state->m17_text_meta));
    res.es = 3U;

    err |= expect_int("reserved null META LSF accepted", m17_apply_lsf_result(state, &res), 1);
    err |= expect_u8("reserved null META preserves expected", state->m17_text_meta_expected_bitmap, 0x0FU);
    err |= expect_u8("reserved null META preserves received", state->m17_text_meta_received_bitmap, 0x02U);
    err |= expect_u8("reserved null META preserves control", state->m17_text_meta_control_or, 0xF2U);
    for (size_t i = 0U; i < sizeof(state->m17_text_meta); i++) {
        err |= expect_u8("reserved null META preserves text", state->m17_text_meta[i], 0x5AU);
    }
    return err;
}

static int
test_stream_dispatch_can_filter_and_aes_gates(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t payload_bits[128];
    uint8_t processed_bits[128];
    uint8_t plaintext_bits[128];
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));

    bytes_to_bits(M17_REF_AES_CIPHERTEXT, payload_bits, sizeof(M17_REF_AES_CIPHERTEXT));
    bytes_to_bits(M17_REF_AES_PLAINTEXT, plaintext_bits, sizeof(M17_REF_AES_PLAINTEXT));
    state->m17_str_dt = 1U;
    state->m17_can = 9U;
    state->m17_can_en = 8;
    state->m17_enc = 2U;
    state->m17_payload_decrypted = 6U;
    DSD_MEMSET(processed_bits, 0xA5U, sizeof(processed_bits));

    int err = 0;
    err |=
        expect_int("CAN filter blocks stream",
                   m17_dispatch_stream_payload(opts, state, payload_bits, M17_REF_AES_TRANSMITTED_FN, processed_bits),
                   M17_STREAM_CAN_FILTERED);
    for (size_t i = 0U; i < sizeof(processed_bits); i++) {
        err |= expect_u8("CAN-filtered stream clears output", processed_bits[i], 0U);
    }
    err |= expect_u8("CAN-filtered stream preserves decrypt flag", state->m17_payload_decrypted, 6U);

    state->m17_can_en = -1;
    state->m17_enc = 2U;
    state->m17_enc_st = 0U;
    DSD_MEMCPY(state->m17_meta, M17_REF_AES_NONCE, sizeof(M17_REF_AES_NONCE));
    err |=
        expect_int("AES missing key locks stream",
                   m17_dispatch_stream_payload(opts, state, payload_bits, M17_REF_AES_TRANSMITTED_FN, processed_bits),
                   M17_STREAM_ENCRYPTED_LOCKED);

    DSD_MEMCPY(state->aes_key, M17_REF_AES128_KEY, sizeof(M17_REF_AES128_KEY));
    state->aes_key_loaded[0] = 1;
    state->aes_key_segments[0] = 2U;
    state->m17_payload_decrypted = 7U;
    err |=
        expect_int("AES stream dispatches",
                   m17_dispatch_stream_payload(opts, state, payload_bits, M17_REF_AES_TRANSMITTED_FN, processed_bits),
                   M17_STREAM_ENCRYPTED_DISPATCHED);
    err |= expect_bytes("AES stream plaintext bits", processed_bits, plaintext_bits, sizeof(plaintext_bits));
    err |= expect_u8("AES transient decrypt flag restored", state->m17_payload_decrypted, 7U);
    return err;
}

static int
test_stream_dispatch_rejects_invalid_arguments_and_unknown_encryption(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t payload_bits[128];
    uint8_t processed_bits[128];
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMSET(payload_bits, 1, sizeof(payload_bits));
    DSD_MEMSET(processed_bits, 0xA5, sizeof(processed_bits));

    int err = 0;
    err |= expect_int("stream null opts rejected",
                      m17_dispatch_stream_payload(NULL, state, payload_bits, M17_REF_STREAM_FN, processed_bits),
                      M17_STREAM_INVALID);
    for (size_t i = 0U; i < sizeof(processed_bits); i++) {
        err |= expect_u8("stream null opts preserves output", processed_bits[i], 0xA5U);
    }

    state->m17_can_en = -1;
    state->m17_enc = 3U;
    state->m17_str_dt = 1U;
    err |= expect_int("unknown encryption locks stream",
                      m17_dispatch_stream_payload(opts, state, payload_bits, M17_REF_STREAM_FN, processed_bits),
                      M17_STREAM_ENCRYPTED_LOCKED);
    for (size_t i = 0U; i < sizeof(processed_bits); i++) {
        err |= expect_u8("unknown encryption clears output", processed_bits[i], 0U);
    }
    err |= expect_u8("unknown encryption leaves decrypt flag clear", state->m17_payload_decrypted, 0U);
    return err;
}

static int
test_stream_dispatch_scrambler_decrypts_with_seed_and_subtype(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t payload_bits[128];
    uint8_t processed_bits[128];
    uint8_t expected_bits[128];
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMSET(payload_bits, 0, sizeof(payload_bits));
    DSD_MEMSET(expected_bits, 0, sizeof(expected_bits));

    for (size_t i = 0U; i < sizeof(payload_bits); i++) {
        payload_bits[i] = (uint8_t)(((i * 3U) + 1U) & 1U);
    }
    const uint8_t subtype = 1U;
    const uint32_t seed = 0xACE1U;
    const uint16_t frame_number = 0x0123U;
    assert(m17_scrambler_apply_bits(subtype, seed, frame_number, payload_bits, expected_bits, M17_STREAM_PAYLOAD_BITS)
           == 0);

    state->m17_can_en = -1;
    state->m17_enc = 1U;
    state->m17_enc_st = subtype;
    state->m17_str_dt = 1U;
    state->R = seed;
    state->m17_payload_decrypted = 9U;

    int err = 0;
    err |= expect_int("scrambler stream dispatches",
                      m17_dispatch_stream_payload(opts, state, payload_bits, frame_number, processed_bits),
                      M17_STREAM_ENCRYPTED_DISPATCHED);
    err |= expect_bytes("scrambler stream output", processed_bits, expected_bits, sizeof(expected_bits));
    err |= expect_u8("scrambler transient decrypt flag restored", state->m17_payload_decrypted, 9U);
    return err;
}

static int
test_stream_signature_frames_are_consumed_and_verify_without_key(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t payload_bits[128];
    uint8_t processed_bits[128];
    static const uint16_t signature_fns[4] = {M17_STREAM_SIGNATURE_FN0, M17_STREAM_SIGNATURE_FN1,
                                              M17_STREAM_SIGNATURE_FN2, M17_STREAM_SIGNATURE_FN3};
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    state->m17_signature_advertised = 1U;
    state->m17_str_dt = 2U;

    int err = 0;
    for (size_t i = 0U; i < 4U; i++) {
        expect_signature_slice(payload_bits, i * 16U);
        err |= expect_int("signature payload consumed",
                          m17_dispatch_stream_payload(opts, state, payload_bits, signature_fns[i], processed_bits),
                          M17_STREAM_SIGNATURE_CONSUMED);
        err |=
            expect_u8("signature received mask", state->m17_signature_received_mask, (uint8_t)((1U << (i + 1U)) - 1U));
        err |= expect_bytes("signature bytes stored", state->m17_signature + (i * 16U),
                            M17_REF_SIGNATURE_BYTES + (i * 16U), 16U);
    }
    err |= expect_u8("signature complete", state->m17_signature_complete, 1U);
    err |= expect_u8("signature sequence ok", state->m17_signature_bad_sequence, 0U);
    err |= expect_u8("signature no public key status", state->m17_signature_verification_status, 4U);
    return err;
}

static int
test_stream_signature_out_of_order_marks_sequence_error(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t payload_bits[128];
    uint8_t processed_bits[128];
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    state->m17_signature_advertised = 1U;
    state->m17_str_dt = 3U;

    expect_signature_slice(payload_bits, 16U);

    int err = 0;
    err |= expect_int("out-of-order signature consumed",
                      m17_dispatch_stream_payload(opts, state, payload_bits, M17_STREAM_SIGNATURE_FN1, processed_bits),
                      M17_STREAM_SIGNATURE_CONSUMED);
    err |= expect_u8("out-of-order signature mask", state->m17_signature_received_mask, 0x02U);
    err |= expect_u8("out-of-order signature sequence", state->m17_signature_bad_sequence, 1U);
    err |= expect_u8("out-of-order signature incomplete", state->m17_signature_complete, 0U);
    return err;
}

static int
test_clear_signed_payload_updates_digest_and_dispatches(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t payload_bits[128];
    uint8_t processed_bits[128];
    uint8_t expected_digest[16];
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    state->m17_signature_advertised = 1U;
    state->m17_str_dt = 1U;
    state->m17_can_en = -1;

    bytes_to_bits(M17_REF_STREAM_PAYLOAD_BYTES, payload_bits, sizeof(M17_REF_STREAM_PAYLOAD_BYTES));
    DSD_MEMCPY(expected_digest, M17_REF_STREAM_PAYLOAD_BYTES, sizeof(expected_digest));
    const uint8_t first = expected_digest[0];
    DSD_MEMMOVE(expected_digest, expected_digest + 1, sizeof(expected_digest) - 1U);
    expected_digest[sizeof(expected_digest) - 1U] = first;

    int err = 0;
    err |= expect_int("clear signed payload dispatches",
                      m17_dispatch_stream_payload(opts, state, payload_bits, M17_REF_STREAM_FN, processed_bits),
                      M17_STREAM_CLEAR_DISPATCHED);
    err |= expect_bytes("clear signed payload preserved", processed_bits, payload_bits, sizeof(payload_bits));
    err |= expect_bytes("clear signed payload digest", state->m17_signature_digest, expected_digest,
                        sizeof(expected_digest));
    err |= expect_u8("clear signed payload decrypt flag restored", state->m17_payload_decrypted, 0U);
    return err;
}

static int
test_stream_voice_replaces_foreign_active_call(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t payload_bits[M17_STREAM_PAYLOAD_BITS];
    uint8_t processed_bits[M17_STREAM_PAYLOAD_BITS];
    dsd_state_ext_free_all(state);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMSET(payload_bits, 0, sizeof(payload_bits));

    const dsd_call_observation foreign = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 1201U,
        .ota_source_id = 4201U,
    };
    int err = 0;
    err |= expect_int("foreign call starts before M17 late entry",
                      dsd_call_state_observe(state, &foreign, DSD_CALL_BOUNDARY_BEGIN), 1);
    dsd_call_snapshot call;
    err |= expect_int("foreign call is available before M17 late entry", get_m17_call(state, &call), 1);
    const uint64_t foreign_epoch = call.epoch;

    state->synctype = DSD_SYNC_M17_STR_POS;
    state->m17_str_dt = 2U;
    state->m17_can_en = -1;
    /* Over the air an LSF CRC clears before the first stream frame; without that the media gate
     * holds the call and the audio back (#399). */
    m17_confirm_note_evidence(state, M17_EVIDENCE_STRONG);
    err |= expect_int("late-entry M17 voice dispatches",
                      m17_dispatch_stream_payload(opts, state, payload_bits, M17_REF_STREAM_FN, processed_bits),
                      M17_STREAM_CLEAR_DISPATCHED);
    err |= expect_int("late-entry M17 voice publishes a call", get_m17_call(state, &call), 1);
    err |= expect_int("late-entry M17 voice remains active", call.phase, DSD_CALL_PHASE_ACTIVE);
    err |= expect_int("late-entry M17 voice replaces foreign epoch", call.epoch != foreign_epoch, 1);
    err |= expect_int("late-entry M17 voice owns canonical protocol", DSD_SYNC_IS_M17(call.protocol), 1);
    err |= expect_int("late-entry M17 voice uses voice kind", call.kind, DSD_CALL_KIND_VOICE);
    err |= expect_u64("late-entry M17 voice clears foreign destination", call.ota_target_id, 0U);
    err |= expect_u64("late-entry M17 voice clears foreign source", call.ota_source_id, 0U);
    err |= expect_u8("late-entry M17 voice marks media", call.media_active, 1U);
    dsd_state_ext_free_all(state);
    return err;
}

#ifdef USE_CODEC2
static int
test_stream_voice_3200_dispatch_routes_pair_audio_to_udp(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    static const uint8_t payload_bytes[16] = {0x10U, 0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U,
                                              0x20U, 0x21U, 0x22U, 0x23U, 0x24U, 0x25U, 0x26U, 0x27U};
    uint8_t payload_bits[128];
    uint8_t processed_bits[128];
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    reset_audio_fakes();
    install_fake_udp_audio();

    opts->slot1_on = 1;
    opts->audio_out = 1;
    opts->audio_out_type = 8;
    state->m17_str_dt = 2U;
    state->m17_can_en = -1;
    /* Over the air an LSF CRC clears before the first stream frame; without that the media gate
     * holds the call and the audio back (#399). */
    m17_confirm_note_evidence(state, M17_EVIDENCE_STRONG);
    bytes_to_bits(payload_bytes, payload_bits, sizeof(payload_bytes));

    int err = 0;
    err |= expect_int("3200 voice stream dispatches",
                      m17_dispatch_stream_payload(opts, state, payload_bits, M17_REF_STREAM_FN, processed_bits),
                      M17_STREAM_CLEAR_DISPATCHED);
    err |= expect_bytes("3200 voice processed bits", processed_bits, payload_bits, sizeof(payload_bits));
    err |= expect_int("3200 voice Codec2 decodes", g_codec2_decode_calls, 2);
    err |= expect_bytes("3200 voice second codec frame", g_codec2_last_bits, payload_bytes + 8U, 8U);
    err |= expect_int("3200 voice UDP calls", g_udp_audio_calls, 2);
    err |= expect_int("3200 voice UDP bytes", (int)g_udp_audio_last_nsam, (int)(160U * sizeof(short)));
    err |= expect_int("3200 voice UDP last sample", (int)g_udp_audio_last_first_sample, 1002);
    err |= expect_int("3200 voice UDP opts", g_udp_audio_last_opts == opts, 1);
    err |= expect_int("3200 voice UDP state", g_udp_audio_last_state == state, 1);
    reset_audio_fakes();
    dsd_state_ext_free_all(state);
    return err;
}

static int
test_stream_voice_1600_dispatch_routes_single_audio_to_udp(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    static const uint8_t payload_bytes[16] = {0x30U, 0x31U, 0x32U, 0x33U, 0x34U, 0x35U, 0x36U, 0x37U,
                                              0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U};
    uint8_t payload_bits[128];
    uint8_t processed_bits[128];
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    reset_audio_fakes();
    install_fake_udp_audio();

    opts->slot1_on = 1;
    opts->audio_out = 1;
    opts->audio_out_type = 8;
    state->m17_str_dt = 3U;
    state->m17_can_en = -1;
    /* Over the air an LSF CRC clears before the first stream frame; without that the media gate
     * holds the call and the audio back (#399). */
    m17_confirm_note_evidence(state, M17_EVIDENCE_STRONG);
    bytes_to_bits(payload_bytes, payload_bits, sizeof(payload_bytes));

    int err = 0;
    err |= expect_int("1600 voice stream dispatches",
                      m17_dispatch_stream_payload(opts, state, payload_bits, M17_REF_STREAM_FN, processed_bits),
                      M17_STREAM_CLEAR_DISPATCHED);
    err |= expect_bytes("1600 voice processed bits", processed_bits, payload_bits, sizeof(payload_bits));
    err |= expect_int("1600 voice Codec2 decodes", g_codec2_decode_calls, 1);
    err |= expect_bytes("1600 voice codec frame", g_codec2_last_bits, payload_bytes, 8U);
    err |= expect_int("1600 voice UDP calls", g_udp_audio_calls, 1);
    err |= expect_int("1600 voice UDP bytes", (int)g_udp_audio_last_nsam, (int)(320U * sizeof(short)));
    err |= expect_int("1600 voice UDP first sample", (int)g_udp_audio_last_first_sample, 1001);
    reset_audio_fakes();
    dsd_state_ext_free_all(state);
    return err;
}

static int
test_stream_voice_audio_gate_suppresses_udp_when_slot_disabled(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    static const uint8_t payload_bytes[16] = {0x40U, 0x41U, 0x42U, 0x43U, 0x44U, 0x45U, 0x46U, 0x47U,
                                              0x50U, 0x51U, 0x52U, 0x53U, 0x54U, 0x55U, 0x56U, 0x57U};
    uint8_t payload_bits[128];
    uint8_t processed_bits[128];
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    reset_audio_fakes();
    install_fake_udp_audio();

    opts->slot1_on = 0;
    opts->audio_out = 1;
    opts->audio_out_type = 8;
    state->m17_str_dt = 2U;
    state->m17_can_en = -1;
    /* Over the air an LSF CRC clears before the first stream frame; without that the media gate
     * holds the call and the audio back (#399). */
    m17_confirm_note_evidence(state, M17_EVIDENCE_STRONG);
    bytes_to_bits(payload_bytes, payload_bits, sizeof(payload_bytes));

    int err = 0;
    err |= expect_int("slot-disabled voice stream dispatches",
                      m17_dispatch_stream_payload(opts, state, payload_bits, M17_REF_STREAM_FN, processed_bits),
                      M17_STREAM_CLEAR_DISPATCHED);
    err |= expect_int("slot-disabled voice still decodes", g_codec2_decode_calls, 2);
    err |= expect_int("slot-disabled voice suppresses UDP", g_udp_audio_calls, 0);
    err |= expect_int("slot-disabled voice leaves UDP byte count clear", (int)g_udp_audio_last_nsam, 0);
    reset_audio_fakes();
    dsd_state_ext_free_all(state);
    return err;
}
#endif

static int
test_bert_payload_locks_from_default_state_and_continues(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t bert_bits[M17_BERT_PAYLOAD_BITS];
    uint16_t tx_lfsr = 1U;
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));

    m17_prbs9_fill_bits(&tx_lfsr, bert_bits, M17_BERT_PAYLOAD_BITS);
    m17_process_bert_payload(opts, state, bert_bits);

    int err = 0;
    err |= expect_u8("BERT locks from default LFSR", state->m17_bert_locked, 1U);
    err |= expect_int("BERT first payload counted bits", (int)state->m17_bert_bits,
                      M17_BERT_PAYLOAD_BITS - M17_PRBS9_LOCK_BITS);
    err |= expect_int("BERT first payload errors", (int)state->m17_bert_errors, 0);
    err |= expect_int("BERT first payload resyncs", (int)state->m17_bert_resyncs, 0);

    m17_prbs9_fill_bits(&tx_lfsr, bert_bits, M17_BERT_PAYLOAD_BITS);
    m17_process_bert_payload(opts, state, bert_bits);

    err |= expect_u8("BERT stays locked", state->m17_bert_locked, 1U);
    err |= expect_int("BERT second payload counted bits", (int)state->m17_bert_bits,
                      (M17_BERT_PAYLOAD_BITS - M17_PRBS9_LOCK_BITS) + M17_BERT_PAYLOAD_BITS);
    err |= expect_int("BERT second payload errors", (int)state->m17_bert_errors, 0);
    err |= expect_int("BERT second payload resyncs", (int)state->m17_bert_resyncs, 0);
    return err;
}

static int
test_bert_payload_resyncs_after_error_threshold(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t bert_bits[M17_BERT_PAYLOAD_BITS];
    uint16_t tx_lfsr = 1U;
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    state->m17_bert_lfsr = 1U;
    state->m17_bert_locked = 1U;

    m17_prbs9_fill_bits(&tx_lfsr, bert_bits, M17_BERT_PAYLOAD_BITS);
    for (int i = 0; i < 19; i++) {
        bert_bits[i] ^= 1U;
    }

    m17_process_bert_payload(opts, state, bert_bits);

    int err = 0;
    err |= expect_u8("BERT relocks after threshold", state->m17_bert_locked, 1U);
    err |= expect_int("BERT resync count", (int)state->m17_bert_resyncs, 1);
    err |= expect_int("BERT threshold counted bits", (int)state->m17_bert_bits,
                      M17_BERT_PAYLOAD_BITS - M17_PRBS9_LOCK_BITS);
    err |= expect_int("BERT threshold errors", (int)state->m17_bert_errors, 19);
    err |= expect_int("BERT window after relock", (int)state->m17_bert_window_bits,
                      M17_BERT_PAYLOAD_BITS - M17_PRBS9_RESYNC_WINDOW_BITS - M17_PRBS9_LOCK_BITS);
    err |= expect_int("BERT window errors reset after resync", (int)state->m17_bert_window_errors, 0);
    return err;
}

static int
test_bert_hard_payload_decode_primitives(void) {
    uint8_t input_bits[M17_PAYLOAD_BITS];
    uint8_t depunc[M17_BERT_TYPE2_BITS];
    uint8_t expected_depunc[M17_BERT_TYPE2_BITS];
    uint8_t bert_bits[M17_BERT_PAYLOAD_BITS];
    uint8_t chainback_bytes[25];
    uint8_t expected_bert_bits[200];
    int bit_in = 0;

    for (int i = 0; i < M17_PAYLOAD_BITS; i++) {
        input_bits[i] = (uint8_t)(((i * 5) + 1) & 1);
    }
    DSD_MEMSET(depunc, 0xA5, sizeof(depunc));
    DSD_MEMSET(expected_depunc, 0, sizeof(expected_depunc));

    m17_depuncture_p2_hard(input_bits, depunc, M17_BERT_TYPE2_BITS);
    for (int i = 0; i < M17_BERT_TYPE2_BITS; i++) {
        if (m17_puncture_pattern_2[i % M17_PUNCTURE_P2_LEN] == 1U && bit_in < M17_PAYLOAD_BITS) {
            expected_depunc[i] = input_bits[bit_in++];
        }
    }

    int err = 0;
    err |= expect_int("BERT depuncture consumes payload", bit_in, M17_PAYLOAD_BITS);
    err |= expect_bytes("BERT depunctured P2 bits", depunc, expected_depunc, sizeof(depunc));

    DSD_MEMSET(depunc, 0x5A, sizeof(depunc));
    m17_depuncture_p2_hard(input_bits, depunc, -1);
    err |= expect_u8("BERT depuncture negative guard preserves output", depunc[0], 0x5AU);

    reset_convolution_fake();
    DSD_MEMSET(bert_bits, 0x5A, sizeof(bert_bits));
    m17_decode_bert_payload_bits(input_bits, bert_bits);
    err |= expect_int("BERT hard decode starts convolution", g_conv_start_calls, 1);
    err |= expect_int("BERT hard decode symbol pairs", g_conv_decode_calls, M17_BERT_TYPE1_FLUSH_BITS);
    err |= expect_int("BERT hard decode chainback calls", g_conv_chainback_calls, 1);
    err |= expect_int("BERT hard decode chainback bits", (int)g_conv_chainback_bits, M17_BERT_PAYLOAD_BITS);
    for (int i = 0; i < (int)sizeof(g_conv_first_symbols); i++) {
        err |= expect_u8("BERT hard decode feeds depunctured soft symbols", g_conv_first_symbols[i],
                         (uint8_t)(expected_depunc[i] << 1U));
    }

    for (size_t i = 0U; i < sizeof(chainback_bytes); i++) {
        chainback_bytes[i] = (uint8_t)(0xA0U + i);
    }
    bytes_to_bits(chainback_bytes, expected_bert_bits, sizeof(chainback_bytes));
    err |=
        expect_bytes("BERT hard decode unpacks chainback bytes", bert_bits, expected_bert_bits, M17_BERT_PAYLOAD_BITS);
    return err;
}

static int
test_frame_info_packet_and_ip_helpers(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));

    int err = 0;
    err |= expect_int("frame info masks fields", m17_compose_frame_info(3U, 7U, 6U, 5U, 0x1FU, 3U, 0x1FU), 0xFFB7);
    err |= expect_int("frame info selected fields", m17_compose_frame_info(1U, 2U, 1U, 3U, 9U, 1U, 0xAU), 0xACED);

    uint8_t ip_frame[12];
    DSD_MEMSET(ip_frame, 0, sizeof(ip_frame));
    ip_frame[4] = 0x01U;
    ip_frame[5] = 0x23U;
    ip_frame[6] = 0x45U;
    ip_frame[7] = 0x67U;
    ip_frame[8] = 0x89U;
    ip_frame[9] = 0xABU;
    err |= expect_u64("IP source", m17_read_ip_source(ip_frame), 0x0123456789ABULL);
    err |= expect_u64("IP null source", m17_read_ip_source(NULL), 0ULL);

    err |= expect_int("pkt negative clamp", m17_pkt_ptr_clamped(-4), 0);
    err |= expect_int("pkt second frame ptr", m17_pkt_ptr_clamped(2), 50);
    err |= expect_int("pkt high clamp", m17_pkt_ptr_clamped(99), 825);

    err |= expect_int("clear packet not encrypted", m17_decode_pkt_should_report_encrypted(state, 0x05U), 0);
    state->m17_enc = 2U;
    err |= expect_int("GNSS meta allowed while encrypted", m17_decode_pkt_should_report_encrypted(state, 0x81U), 0);
    err |= expect_int("SMS blocked while encrypted", m17_decode_pkt_should_report_encrypted(state, 0x05U), 1);
    state->m17_payload_decrypted = 1U;
    err |= expect_int("decrypted packet allowed", m17_decode_pkt_should_report_encrypted(state, 0x05U), 0);

    DSD_MEMSET(state, 0, sizeof(*state));
    state->carrier = 1;
    state->synctype = DSD_SYNC_M17_STR_POS;
    uint8_t disc[10] = {'D', 'I', 'S', 'C', 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB};
    m17_ip_dispatch_frame(opts, state, disc, sizeof(disc));
    err |= expect_int("DISC drops carrier", state->carrier, 0);
    err |= expect_int("DISC clears synctype", state->synctype, DSD_SYNC_NONE);

    state->carrier = 1;
    state->synctype = DSD_SYNC_M17_STR_POS;
    uint8_t eotx[10] = {'E', 'O', 'T', 'X', 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB};
    m17_ip_dispatch_frame(opts, state, eotx, sizeof(eotx));
    err |= expect_int("EOTX drops carrier", state->carrier, 0);
    err |= expect_int("EOTX clears synctype", state->synctype, DSD_SYNC_NONE);

    state->carrier = 1;
    state->synctype = DSD_SYNC_M17_STR_POS;
    uint8_t conn[11] = {'C', 'O', 'N', 'N', 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 'A'};
    m17_ip_dispatch_frame(opts, state, conn, sizeof(conn));
    err |= expect_int("CONN keeps carrier", state->carrier, 1);
    err |= expect_int("CONN keeps synctype", state->synctype, DSD_SYNC_M17_STR_POS);

    uint8_t ping[10] = {'P', 'I', 'N', 'G', 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB};
    m17_ip_dispatch_frame(opts, state, ping, sizeof(ping));
    err |= expect_int("PING keeps carrier", state->carrier, 1);
    err |= expect_int("PING keeps synctype", state->synctype, DSD_SYNC_M17_STR_POS);

    uint8_t pong[10] = {'P', 'O', 'N', 'G', 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB};
    m17_ip_dispatch_frame(opts, state, pong, sizeof(pong));
    err |= expect_int("PONG keeps carrier", state->carrier, 1);
    err |= expect_int("PONG keeps synctype", state->synctype, DSD_SYNC_M17_STR_POS);

    uint8_t short_mpkt[12] = {'M', 'P', 'K', 'T', 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0x55, 0xAA};
    m17_ip_dispatch_frame(opts, state, short_mpkt, sizeof(short_mpkt));
    dsd_call_snapshot call;
    err |= expect_int("short MPKT does not publish call", get_m17_call(state, &call), 0);

    uint8_t unknown[10] = {'N', 'O', 'P', 'E', 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB};
    state->carrier = 1;
    state->synctype = DSD_SYNC_M17_STR_POS;
    m17_ip_dispatch_frame(opts, state, unknown, sizeof(unknown));
    err |= expect_int("unknown IP magic preserves carrier", state->carrier, 1);
    err |= expect_int("unknown IP magic preserves synctype", state->synctype, DSD_SYNC_M17_STR_POS);
    return err;
}

static int
test_ip_stream_frames_apply_crc_gated_lsf_state(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t payload_bits[M17_STREAM_PAYLOAD_BITS];
    uint8_t ip_frame[54];
    uint8_t counter[M17_AES_COUNTER_BYTES];
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMSET(payload_bits, 0, sizeof(payload_bits));
    DSD_MEMSET(counter, 0, sizeof(counter));

    const unsigned long long dst = m17_encode_b40_callsign(0ULL, M17_REF_LSF_DST_CSD);
    const unsigned long long src = m17_encode_b40_callsign(0ULL, M17_REF_LSF_SRC_CSD);
    const uint16_t type_word = m17_compose_frame_info(1U, 1U, 2U, 0U, 5U, 0U, 0U);
    build_lsf_bits(lsf_bits, dst, src, type_word, M17_REF_AES_NONCE);
    bytes_to_bits(M17_REF_STREAM_PAYLOAD_BYTES, payload_bits, sizeof(M17_REF_STREAM_PAYLOAD_BYTES));
    build_ip_stream_frame(ip_frame, lsf_bits, 0xBEEF, M17_REF_STREAM_FN, 0U, payload_bits);

    state->m17_can_en = -1;
    m17_ip_dispatch_frame(opts, state, ip_frame, sizeof(ip_frame));

    int err = 0;
    err |= expect_int("valid IP stream sets carrier", state->carrier, 1);
    err |= expect_int("valid IP stream sets synctype", state->synctype, DSD_SYNC_M17_STR_POS);
    dsd_call_snapshot call;
    err |= expect_int("data IP stream publishes identity", get_m17_call(state, &call), 1);
    err |= expect_int("data IP stream publishes data kind", call.kind, DSD_CALL_KIND_DATA);
    err |= expect_u64("data IP stream destination", call.ota_target_id, dst);
    err |= expect_u64("data IP stream source", call.ota_source_id, src);
    err |= expect_u8("valid IP stream data type", state->m17_str_dt, 1U);
    err |= expect_u8("valid IP stream CAN", state->m17_can, 5U);
    err |= expect_u8("valid IP stream AES mode", state->m17_enc, 2U);
    err |= expect_u8("valid IP stream AES subtype", state->m17_enc_st, 0U);
    err |= expect_u8("valid IP stream remains locked without key", state->m17_payload_decrypted, 0U);
    err |= expect_bytes("valid IP stream nonce", state->m17_meta, M17_REF_AES_NONCE, sizeof(M17_REF_AES_NONCE));
    for (size_t i = 0U; i < M17_LSF_LSD_BITS; i++) {
        err |= expect_u8("valid IP stream copied LSF bits", state->m17_lsf[i], lsf_bits[i]);
    }

    const uint64_t active_epoch = call.epoch;
    build_ip_stream_frame(ip_frame, lsf_bits, 0xBEEF, (uint16_t)(M17_REF_STREAM_FN + 1U), 1U, payload_bits);
    ip_frame[52] ^= 0x01U;
    m17_ip_dispatch_frame(opts, state, ip_frame, sizeof(ip_frame));
    err |= expect_int("bad-CRC EOT keeps IP stream call", get_m17_call(state, &call), 1);
    err |= expect_int("bad-CRC EOT keeps IP stream active", call.phase, DSD_CALL_PHASE_ACTIVE);
    err |= expect_u64("bad-CRC EOT keeps IP stream epoch", call.epoch, active_epoch);

    build_ip_stream_frame(ip_frame, lsf_bits, 0xBEEF, (uint16_t)(M17_REF_STREAM_FN + 2U), 0U, payload_bits);
    m17_ip_dispatch_frame(opts, state, ip_frame, sizeof(ip_frame));
    err |= expect_int("valid frame after bad EOT keeps IP stream call", get_m17_call(state, &call), 1);
    err |= expect_int("valid frame after bad EOT keeps IP stream active", call.phase, DSD_CALL_PHASE_ACTIVE);
    err |= expect_u64("valid frame after bad EOT keeps IP stream epoch", call.epoch, active_epoch);

    dsd_state_ext_free_all(state);
    DSD_MEMSET(state, 0, sizeof(*state));
    state->m17_can_en = -1;
    state->m17_str_dt = 1U;
    state->m17_enc = 0U;
    DSD_MEMSET(state->m17_meta, 0x5A, sizeof(state->m17_meta));
    m17_aes_build_counter(state->m17_meta, (uint16_t)(M17_STREAM_FRAME_END_MASK | M17_REF_STREAM_FN), counter);
    build_ip_stream_frame(ip_frame, lsf_bits, 0xBEEF, M17_REF_STREAM_FN, 1U, payload_bits);
    ip_frame[52] ^= 0x01U;

    m17_ip_dispatch_frame(opts, state, ip_frame, sizeof(ip_frame));
    err |= expect_int("bad IP stream CRC sets carrier", state->carrier, 1);
    err |= expect_int("bad IP stream CRC sets synctype", state->synctype, DSD_SYNC_M17_STR_POS);
    err |= expect_int("bad data IP stream CRC does not publish call", get_m17_call(state, &call), 0);
    err |= expect_u8("bad IP stream CRC preserves enc", state->m17_enc, 0U);
    err |= expect_u8("bad IP stream CRC stores counter high", state->m17_meta[14], counter[14]);
    err |= expect_u8("bad IP stream CRC stores counter low", state->m17_meta[15], counter[15]);
    return err;
}

static int
test_ip_stream_bad_crc_does_not_reopen_ended_voice_epoch(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t payload_bits[M17_STREAM_PAYLOAD_BITS];
    uint8_t ip_frame[54];
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMSET(payload_bits, 0, sizeof(payload_bits));

    const unsigned long long dst = m17_encode_b40_callsign(0ULL, M17_REF_LSF_DST_CSD);
    const unsigned long long src = m17_encode_b40_callsign(0ULL, M17_REF_LSF_SRC_CSD);
    const uint16_t type_word = m17_compose_frame_info(1U, 2U, 0U, 0U, 5U, 0U, 0U);
    build_lsf_bits(lsf_bits, dst, src, type_word, NULL);
    build_ip_stream_frame(ip_frame, lsf_bits, 0xBEEFU, M17_REF_STREAM_FN, 1U, payload_bits);

    state->m17_can_en = -1;
    m17_ip_dispatch_frame(opts, state, ip_frame, sizeof(ip_frame));

    int err = 0;
    dsd_call_snapshot call;
    err |= expect_int("valid voice EOT publishes call", get_m17_call(state, &call), 1);
    err |= expect_int("valid voice EOT ends call", call.phase, DSD_CALL_PHASE_ENDED);
    err |= expect_int("valid voice EOT owns M17 protocol", DSD_SYNC_IS_M17(call.protocol), 1);
    // Terminator, not EXPLICIT: an over-the-air EOT is positive end evidence, and the event
    // layer keys its keep-or-drop verdict for identity-less audible rows on that distinction.
    err |= expect_int("valid voice EOT records terminator end", call.end_reason, (int)DSD_CALL_END_TERMINATOR);
    const uint64_t ended_epoch = call.epoch;

    build_ip_stream_frame(ip_frame, lsf_bits, 0xBEEFU, (uint16_t)(M17_REF_STREAM_FN + 1U), 0U, payload_bits);
    ip_frame[52] ^= 0x01U;
    m17_ip_dispatch_frame(opts, state, ip_frame, sizeof(ip_frame));

    err |= expect_int("bad-CRC voice frame retains call", get_m17_call(state, &call), 1);
    err |= expect_int("bad-CRC voice frame keeps call ended", call.phase, DSD_CALL_PHASE_ENDED);
    err |= expect_u64("bad-CRC voice frame preserves epoch", call.epoch, ended_epoch);
    err |= expect_u8("bad-CRC voice frame leaves media inactive", call.media_active, 0U);
    dsd_state_ext_free_all(state);
    return err;
}

static int
test_ip_mpkt_frames_apply_crc_gated_packet_state(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t mpkt_frame[80];
    uint8_t app[2U + M17_META_BYTES];
    uint8_t expected_text[M17_TEXT_BLOCK_BYTES];
    static Event_History_I event_history[2];
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMSET(app, 0, sizeof(app));
    DSD_MEMSET(expected_text, 0, sizeof(expected_text));
    DSD_MEMSET(event_history, 0, sizeof(event_history));
    for (uint8_t slot = 0U; slot < 2U; slot++) {
        init_event_history(&event_history[slot], 0, 255);
    }
    state->event_history_s = event_history;

    const unsigned long long dst = m17_encode_b40_callsign(0ULL, M17_REF_LSF_DST_CSD);
    const unsigned long long src = m17_encode_b40_callsign(0ULL, M17_REF_LSF_SRC_CSD);
    const uint16_t packet_type_word = m17_compose_frame_info(0U, 0U, 0U, 0U, 5U, 0U, 0U);
    build_lsf_bits(lsf_bits, dst, src, packet_type_word, NULL);

    app[0] = 0xC2U;
    app[1] = 0x80U;
    app[2] = 0x11U;
    DSD_MEMCPY(app + 3, "IP-MPKT-OK", 10U);
    DSD_MEMCPY(expected_text, "IP-MPKT-OK", 10U);
    const size_t mpkt_len = build_ip_mpkt_frame(mpkt_frame, sizeof(mpkt_frame), lsf_bits, 0xCAFEU, app, sizeof(app));

    state->m17_can_en = -1;
    m17_ip_dispatch_frame(opts, state, mpkt_frame, (int)mpkt_len);

    int err = 0;
    dsd_call_snapshot call;
    err |= expect_int("valid MPKT publishes identity", get_m17_call(state, &call), 1);
    err |= expect_int("valid MPKT publishes data kind", call.kind, DSD_CALL_KIND_DATA);
    err |= expect_u64("valid MPKT destination", call.ota_target_id, dst);
    err |= expect_u64("valid MPKT source", call.ota_source_id, src);
    err |= expect_int("valid MPKT event destination",
                      strcmp(event_history[0].Event_History_Items[1].tgt_str, M17_REF_LSF_DST_CSD), 0);
    err |= expect_int("valid MPKT event source",
                      strcmp(event_history[0].Event_History_Items[1].src_str, M17_REF_LSF_SRC_CSD), 0);
    err |= expect_int("valid MPKT ends canonical call", call.phase, DSD_CALL_PHASE_ENDED);
    err |= expect_int("valid MPKT clears current event",
                      event_history[0].Event_History_Items[0].event_string[0] == '\0', 1);
    err |= expect_int("valid MPKT commits event history",
                      event_history[0].Event_History_Items[1].event_string[0] != '\0', 1);
    err |= expect_u8("valid MPKT packet mode data type", state->m17_str_dt, 20U);
    err |= expect_u8("valid MPKT CAN", state->m17_can, 5U);
    err |= expect_u8("valid MPKT clear encryption", state->m17_enc, 0U);
    err |= expect_u8("valid MPKT text expected bitmap", state->m17_text_meta_expected_bitmap, 0x01U);
    err |= expect_u8("valid MPKT text received bitmap", state->m17_text_meta_received_bitmap, 0x01U);
    err |= expect_u8("valid MPKT text control", state->m17_text_meta_control_or, 0x11U);
    err |= expect_bytes("valid MPKT text bytes", state->m17_text_meta, expected_text, sizeof(expected_text));

    const uint64_t first_epoch = call.epoch;
    m17_ip_dispatch_frame(opts, state, mpkt_frame, (int)mpkt_len);
    err |= expect_int("next matching MPKT retains canonical call", get_m17_call(state, &call), 1);
    err |= expect_int("next matching MPKT ends canonical call", call.phase, DSD_CALL_PHASE_ENDED);
    err |= expect_int("next matching MPKT uses distinct epoch", call.epoch != first_epoch, 1);
    err |= expect_int("next matching MPKT clears current event",
                      event_history[0].Event_History_Items[0].event_string[0] == '\0', 1);

    dsd_state_ext_free_all(state);
    DSD_MEMSET(state, 0, sizeof(*state));
    state->event_history_s = event_history;
    state->m17_can_en = -1;
    state->m17_text_meta_expected_bitmap = 0x0FU;
    state->m17_text_meta_received_bitmap = 0x02U;
    state->m17_text_meta_control_or = 0xF2U;
    DSD_MEMSET(state->m17_text_meta, 0x5AU, sizeof(state->m17_text_meta));
    build_ip_mpkt_frame(mpkt_frame, sizeof(mpkt_frame), lsf_bits, 0xCAFEU, app, sizeof(app));
    mpkt_frame[mpkt_len - 1U] ^= 0x01U;

    m17_ip_dispatch_frame(opts, state, mpkt_frame, (int)mpkt_len);
    err |= expect_int("bad MPKT CRC does not publish call", get_m17_call(state, &call), 0);
    err |= expect_u8("bad MPKT CRC preserves text expected", state->m17_text_meta_expected_bitmap, 0x0FU);
    err |= expect_u8("bad MPKT CRC preserves text received", state->m17_text_meta_received_bitmap, 0x02U);
    err |= expect_u8("bad MPKT CRC preserves text control", state->m17_text_meta_control_or, 0xF2U);
    for (size_t i = 0U; i < sizeof(state->m17_text_meta); i++) {
        err |= expect_u8("bad MPKT CRC preserves text bytes", state->m17_text_meta[i], 0x5AU);
    }
    dsd_state_ext_free_all(state);
    return err;
}

static void
build_packet_text_meta_app(uint8_t app[2U + M17_META_BYTES], const char text[M17_TEXT_BLOCK_BYTES]) {
    DSD_MEMSET(app, 0, 2U + M17_META_BYTES);
    app[0] = 0xC2U;
    app[1] = 0x80U;
    app[2] = 0x11U;
    DSD_MEMCPY(app + 3, text, M17_TEXT_BLOCK_BYTES);
}

static void
stage_packet_with_crc(dsd_state* state, const uint8_t* app, uint16_t app_len, uint16_t crc) {
    DSD_MEMSET(state->m17_pkt, 0, sizeof(state->m17_pkt));
    DSD_MEMCPY(state->m17_pkt, app, app_len);
    state->m17_pkt[app_len] = (uint8_t)(crc >> 8U);
    state->m17_pkt[app_len + 1U] = (uint8_t)(crc & 0xFFU);
}

static uint64_t
start_m17_packet_call(dsd_opts* opts, dsd_state* state, Event_History_I event_history[2]) {
    DSD_MEMSET(event_history, 0, sizeof(Event_History_I) * 2U);
    for (uint8_t slot = 0U; slot < 2U; slot++) {
        init_event_history(&event_history[slot], 0, 255);
    }
    state->event_history_s = event_history;
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_M17_LSF_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_DATA,
        .ota_target_id = 0xFFFFFFFFFFFFULL,
        .ota_source_id = 0x000000000001ULL,
        .source_text = "M17SRC",
        .target_text = "BROADCAST",
    };
    if (dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_CONTINUE) <= 0) {
        return 0U;
    }
    dsd_event_sync_slot(opts, state, 0U);
    dsd_call_snapshot call;
    return dsd_call_state_get(state, 0U, &call) > 0 ? call.epoch : 0U;
}

static int
test_packet_eot_finalization_crc_gates_decode_and_clears_state(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t app[2U + M17_META_BYTES];
    uint8_t expected_text[M17_TEXT_BLOCK_BYTES];
    static Event_History_I event_history[2];
    static const char text[M17_TEXT_BLOCK_BYTES] = {'R', 'F', '-', 'P', 'K', 'T', '-', 'O', 'K', 0, 0, 0, 0};
    dsd_state_ext_free_all(state);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMSET(expected_text, 0, sizeof(expected_text));
    build_packet_text_meta_app(app, text);
    DSD_MEMCPY(expected_text, text, sizeof(expected_text));

    const uint16_t app_len = (uint16_t)sizeof(app);
    const size_t total_len = (size_t)app_len + (size_t)M17_PACKET_CRC_BYTES;
    const uint16_t crc = m17_crc16(app, app_len);
    state->m17_can_en = -1;
    state->m17_pbc_ct = 4;
    const uint64_t first_epoch = start_m17_packet_call(opts, state, event_history);
    stage_packet_with_crc(state, app, app_len, crc);

    m17_pkt_finalize_eot(opts, state, app_len, (int)(app_len + M17_PACKET_CRC_BYTES));

    int err = 0;
    err |= expect_int("valid packet EOT starts canonical epoch", first_epoch != 0U, 1);
    err |= expect_u8("valid packet EOT text expected bitmap", state->m17_text_meta_expected_bitmap, 0x01U);
    err |= expect_u8("valid packet EOT text received bitmap", state->m17_text_meta_received_bitmap, 0x01U);
    err |= expect_u8("valid packet EOT text control", state->m17_text_meta_control_or, 0x11U);
    err |= expect_bytes("valid packet EOT text bytes", state->m17_text_meta, expected_text, sizeof(expected_text));
    err |= expect_int("valid packet EOT clears PBC", state->m17_pbc_ct, 0);
    for (size_t i = 0U; i < total_len; i++) {
        err |= expect_u8("valid packet EOT clears packet buffer", state->m17_pkt[i], 0U);
    }
    dsd_call_snapshot call;
    err |= expect_int("valid packet EOT retains canonical call", get_m17_call(state, &call), 1);
    err |= expect_int("valid packet EOT ends canonical call", call.phase, DSD_CALL_PHASE_ENDED);
    err |= expect_u64("valid packet EOT preserves canonical epoch", call.epoch, first_epoch);
    err |= expect_int("valid packet EOT clears current event",
                      event_history[0].Event_History_Items[0].event_string[0] == '\0', 1);
    err |= expect_int("valid packet EOT commits event history",
                      event_history[0].Event_History_Items[1].event_string[0] != '\0', 1);

    const dsd_call_observation next_packet = {
        .protocol = DSD_SYNC_M17_LSF_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_DATA,
        .ota_target_id = 0xFFFFFFFFFFFFULL,
        .ota_source_id = 0x000000000001ULL,
        .source_text = "M17SRC",
        .target_text = "BROADCAST",
    };
    err |= expect_int("next matching packet starts canonical epoch",
                      dsd_call_state_observe(state, &next_packet, DSD_CALL_BOUNDARY_CONTINUE), 1);
    err |= expect_int("next matching packet retains canonical call", get_m17_call(state, &call), 1);
    err |= expect_int("next matching packet is active", call.phase, DSD_CALL_PHASE_ACTIVE);
    err |= expect_int("next matching packet uses distinct epoch", call.epoch != first_epoch, 1);

    dsd_state_ext_free_all(state);
    DSD_MEMSET(state, 0, sizeof(*state));
    state->m17_can_en = -1;
    state->m17_pbc_ct = 2;
    state->m17_text_meta_expected_bitmap = 0x0FU;
    state->m17_text_meta_received_bitmap = 0x02U;
    state->m17_text_meta_control_or = 0xF2U;
    DSD_MEMSET(state->m17_text_meta, 0x5AU, sizeof(state->m17_text_meta));
    opts->aggressive_framesync = 1;
    stage_packet_with_crc(state, app, app_len, (uint16_t)(crc ^ 0x0001U));

    m17_pkt_finalize_eot(opts, state, app_len, (int)(app_len + M17_PACKET_CRC_BYTES));

    err |= expect_u8("bad packet CRC preserves text expected", state->m17_text_meta_expected_bitmap, 0x0FU);
    err |= expect_u8("bad packet CRC preserves text received", state->m17_text_meta_received_bitmap, 0x02U);
    err |= expect_u8("bad packet CRC preserves text control", state->m17_text_meta_control_or, 0xF2U);
    for (size_t i = 0U; i < sizeof(state->m17_text_meta); i++) {
        err |= expect_u8("bad packet CRC preserves text bytes", state->m17_text_meta[i], 0x5AU);
    }
    err |= expect_int("bad packet CRC clears PBC", state->m17_pbc_ct, 0);
    for (size_t i = 0U; i < total_len; i++) {
        err |= expect_u8("bad packet CRC clears packet buffer", state->m17_pkt[i], 0U);
    }

    dsd_state_ext_free_all(state);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    state->m17_can_en = -1;
    stage_packet_with_crc(state, app, app_len, (uint16_t)(crc ^ 0x0001U));

    m17_pkt_finalize_eot(opts, state, app_len, (int)(app_len + M17_PACKET_CRC_BYTES));

    err |=
        expect_u8("non-aggressive bad CRC still decodes expected bitmap", state->m17_text_meta_expected_bitmap, 0x01U);
    err |=
        expect_u8("non-aggressive bad CRC still decodes received bitmap", state->m17_text_meta_received_bitmap, 0x01U);
    err |= expect_u8("non-aggressive bad CRC still decodes control", state->m17_text_meta_control_or, 0x11U);
    err |=
        expect_bytes("non-aggressive bad CRC text bytes", state->m17_text_meta, expected_text, sizeof(expected_text));
    dsd_state_ext_free_all(state);
    return err;
}

static int
test_packet_eot_preserves_non_packet_calls(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t app[2U + M17_META_BYTES];
    static const char text[M17_TEXT_BLOCK_BYTES] = {'R', 'F', '-', 'P', 'K', 'T', '-', 'E', 'O', 'T', 0, 0, 0};
    dsd_state_ext_free_all(state);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    build_packet_text_meta_app(app, text);
    const uint16_t app_len = (uint16_t)sizeof(app);
    const uint16_t crc = m17_crc16(app, app_len);

    const dsd_call_observation foreign = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 1301U,
        .ota_source_id = 4301U,
    };
    int err = 0;
    err |= expect_int("foreign call starts before packet EOT",
                      dsd_call_state_observe(state, &foreign, DSD_CALL_BOUNDARY_BEGIN), 1);
    dsd_call_snapshot call;
    err |= expect_int("foreign call is available before packet EOT", get_m17_call(state, &call), 1);
    const uint64_t foreign_epoch = call.epoch;
    stage_packet_with_crc(state, app, app_len, crc);

    m17_pkt_finalize_eot(opts, state, app_len, (int)(app_len + M17_PACKET_CRC_BYTES));

    err |= expect_int("packet EOT preserves foreign call", get_m17_call(state, &call), 1);
    err |= expect_int("packet EOT keeps foreign call active", call.phase, DSD_CALL_PHASE_ACTIVE);
    err |= expect_u64("packet EOT keeps foreign epoch", call.epoch, foreign_epoch);
    err |= expect_int("packet EOT keeps foreign protocol", call.protocol, DSD_SYNC_DMR_BS_VOICE_POS);
    err |= expect_u64("packet EOT keeps foreign destination", call.ota_target_id, 1301U);
    err |= expect_u64("packet EOT keeps foreign source", call.ota_source_id, 4301U);

    const dsd_call_observation m17_voice = {
        .protocol = DSD_SYNC_M17_STR_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_VOICE,
        .ota_target_id = 2301U,
        .ota_source_id = 5301U,
    };
    err |= expect_int("M17 voice starts before packet EOT",
                      dsd_call_state_observe(state, &m17_voice, DSD_CALL_BOUNDARY_BEGIN), 1);
    err |= expect_int("M17 voice is available before packet EOT", get_m17_call(state, &call), 1);
    const uint64_t voice_epoch = call.epoch;
    stage_packet_with_crc(state, app, app_len, crc);

    m17_pkt_finalize_eot(opts, state, app_len, (int)(app_len + M17_PACKET_CRC_BYTES));

    err |= expect_int("packet EOT preserves M17 voice", get_m17_call(state, &call), 1);
    err |= expect_int("packet EOT keeps M17 voice active", call.phase, DSD_CALL_PHASE_ACTIVE);
    err |= expect_u64("packet EOT keeps M17 voice epoch", call.epoch, voice_epoch);
    err |= expect_int("packet EOT keeps M17 voice kind", call.kind, DSD_CALL_KIND_VOICE);
    err |= expect_u64("packet EOT keeps M17 voice destination", call.ota_target_id, 2301U);
    err |= expect_u64("packet EOT keeps M17 voice source", call.ota_source_id, 5301U);
    dsd_state_ext_free_all(state);
    return err;
}

static int
test_encoder_control_lsf_and_dibit_helpers(void) {
    uint8_t conn[11];
    uint8_t disc[10];
    uint8_t eotx[10];
    uint8_t lsf_bits[240];
    uint8_t lsf_packed[30];
    DSD_MEMSET(conn, 0xA5, sizeof(conn));
    DSD_MEMSET(disc, 0xA5, sizeof(disc));
    DSD_MEMSET(eotx, 0xA5, sizeof(eotx));
    DSD_MEMSET(lsf_bits, 0, sizeof(lsf_bits));
    DSD_MEMSET(lsf_packed, 0, sizeof(lsf_packed));

    m17_setup_conn_disc_eotx(0x0123456789ABULL, 'Z', conn, disc, eotx);

    int err = 0;
    static const uint8_t want_conn[11] = {'C', 'O', 'N', 'N', 0x01U, 0x23U, 0x45U, 0x67U, 0x89U, 0xABU, 'Z'};
    static const uint8_t want_disc[10] = {'D', 'I', 'S', 'C', 0x01U, 0x23U, 0x45U, 0x67U, 0x89U, 0xABU};
    static const uint8_t want_eotx[10] = {'E', 'O', 'T', 'X', 0x01U, 0x23U, 0x45U, 0x67U, 0x89U, 0xABU};
    err |= expect_bytes("CONN frame", conn, want_conn, sizeof(want_conn));
    err |= expect_bytes("DISC frame", disc, want_disc, sizeof(want_disc));
    err |= expect_bytes("EOTX frame", eotx, want_eotx, sizeof(want_eotx));

    const unsigned long long dst = m17_encode_b40_callsign(0ULL, M17_REF_LSF_DST_CSD);
    const unsigned long long src = m17_encode_b40_callsign(0ULL, M17_REF_LSF_SRC_CSD);
    m17_load_lsf_callsigns(lsf_bits, dst, src);
    const uint16_t type_word = m17_compose_frame_info(1U, 2U, 0U, 0U, 9U, 1U, 0U);
    for (int i = 0; i < 16; i++) {
        lsf_bits[96 + i] = (uint8_t)((type_word >> (15 - i)) & 1U);
    }
    const uint16_t crc = m17_attach_lsf_crc(lsf_bits, lsf_packed);
    err |= expect_u64("LSF dst bits", bits_to_u64(lsf_bits, 48U), dst);
    err |= expect_u64("LSF src bits", bits_to_u64(lsf_bits + 48, 48U), src);
    err |= expect_int("LSF type word", (int)bits_to_u64(lsf_bits + 96, 16U), type_word);
    err |= expect_int("LSF packed CRC", crc, m17_crc16(lsf_packed, M17_LSF_LSD_BYTES));
    err |= expect_int("LSF CRC bits", (int)bits_to_u64(lsf_bits + M17_LSF_LSD_BITS, M17_LSF_CRC_BITS), crc);

    err |= expect_int("clip high", m17_clip_float_to_short(40000.0f), 32767);
    err |= expect_int("clip low", m17_clip_float_to_short(-40000.0f), -32768);
    err |= expect_int("clip rounded positive", m17_clip_float_to_short(123.6f), 124);
    err |= expect_int("clip rounded negative", m17_clip_float_to_short(-123.4f), -123);

    uint8_t symbol_dibits[M17_FRAME_SYMBOLS];
    int symbols[M17_FRAME_SYMBOLS];
    int upsampled[M17_FRAME_SYMBOLS * M17_RECOMMENDED_UPSAMPLE_FACTOR];
    short baseband[M17_FRAME_SYMBOLS * M17_RECOMMENDED_UPSAMPLE_FACTOR];
    DSD_MEMSET(symbol_dibits, 0, sizeof(symbol_dibits));
    DSD_MEMSET(symbols, 0, sizeof(symbols));
    DSD_MEMSET(upsampled, 0, sizeof(upsampled));
    DSD_MEMSET(baseband, 0, sizeof(baseband));
    symbol_dibits[0] = 0U;
    symbol_dibits[1] = 1U;
    symbol_dibits[2] = 2U;
    symbol_dibits[3] = 3U;
    symbol_dibits[4] = 7U;
    m17_dibits_to_symbols(symbol_dibits, symbols);
    err |= expect_int("dibit 0 symbol", symbols[0], 1);
    err |= expect_int("dibit 1 symbol", symbols[1], 3);
    err |= expect_int("dibit 2 symbol", symbols[2], -1);
    err |= expect_int("dibit 3 symbol", symbols[3], -3);
    err |= expect_int("dibit masked symbol", symbols[4], -3);

    m17_upsample_symbols_10x(symbols, upsampled);
    for (int i = 0; i < M17_RECOMMENDED_UPSAMPLE_FACTOR; i++) {
        err |= expect_int("first symbol upsampled", upsampled[i], 1);
        err |= expect_int("second symbol upsampled", upsampled[M17_RECOMMENDED_UPSAMPLE_FACTOR + i], 3);
    }

    m17_baseband_no_filter(upsampled, baseband);
    err |= expect_int("baseband symbol +1", baseband[0], 7168);
    err |= expect_int("baseband symbol +3", baseband[M17_RECOMMENDED_UPSAMPLE_FACTOR], 21504);
    symbols[0] = -1;
    symbols[1] = -3;
    DSD_MEMSET(upsampled, 0, sizeof(upsampled));
    DSD_MEMSET(baseband, 0, sizeof(baseband));
    m17_upsample_symbols_10x(symbols, upsampled);
    m17_baseband_no_filter(upsampled, baseband);
    err |= expect_int("baseband symbol -1", baseband[0], -7168);
    err |= expect_int("baseband symbol -3", baseband[M17_RECOMMENDED_UPSAMPLE_FACTOR], -21504);

    DSD_MEMSET(symbol_dibits, 0x22, sizeof(symbol_dibits));
    for (size_t i = 0U; i < (M17_FRAME_SYMBOLS * M17_RECOMMENDED_UPSAMPLE_FACTOR); i++) {
        baseband[i] = 55;
    }
    m17_maybe_apply_dead_air(1, symbol_dibits, baseband);
    err |= expect_u8("non-dead-air preserves dibit", symbol_dibits[0], 0x22U);
    err |= expect_int("non-dead-air preserves baseband", baseband[0], 55);
    m17_maybe_apply_dead_air(99, symbol_dibits, baseband);
    for (int i = 0; i < M17_FRAME_SYMBOLS; i++) {
        err |= expect_u8("dead-air dibit marker", symbol_dibits[i], 0xFFU);
    }
    for (size_t i = 0U; i < (M17_FRAME_SYMBOLS * M17_RECOMMENDED_UPSAMPLE_FACTOR); i++) {
        err |= expect_int("dead-air baseband silence", baseband[i], 0);
    }

    uint8_t bert_bits[M17_BERT_PAYLOAD_BITS];
    uint8_t reversed[208];
    for (int i = 0; i < M17_BERT_PAYLOAD_BITS; i++) {
        bert_bits[i] = (uint8_t)((i % 3) == 0);
    }
    DSD_MEMSET(reversed, 0xA5, sizeof(reversed));
    m17_reverse_brt_bits(bert_bits, reversed);
    err |= expect_u8("BERT reverse leading zero 0", reversed[0], 0U);
    err |= expect_u8("BERT reverse leading zero 1", reversed[1], 0U);
    err |= expect_u8("BERT reverse leading zero 2", reversed[2], 0U);
    for (int i = 0; i < M17_BERT_PAYLOAD_BITS; i++) {
        err |= expect_u8("BERT reverse payload", reversed[i + 3], bert_bits[(M17_BERT_PAYLOAD_BITS - 1) - i]);
    }
    err |= expect_u8("BERT reverse trailing zero", reversed[200], 0U);

    return err;
}

static int
test_packet_protocol_identifier_utf8_boundaries(void) {
    uint8_t out[4];
    int err = 0;

    DSD_MEMSET(out, 0xA5, sizeof(out));
    err |= expect_int("packet protocol null guard", (int)m17_encode_packet_protocol_id(0x05U, NULL), 0);
    err |= expect_int("packet protocol high guard",
                      (int)m17_encode_packet_protocol_id(M17_PACKET_PROTOCOL_MAX + 1U, out), 0);
    for (size_t i = 0U; i < sizeof(out); i++) {
        err |= expect_u8("packet protocol invalid preserves output", out[i], 0xA5U);
    }

    static const uint8_t want_sms[4] = {0x05U, 0xA5U, 0xA5U, 0xA5U};
    err |= expect_int("packet protocol one byte", (int)m17_encode_packet_protocol_id(0x05U, out), 1);
    err |= expect_bytes("packet protocol one-byte layout", out, want_sms, sizeof(want_sms));

    DSD_MEMSET(out, 0xA5, sizeof(out));
    static const uint8_t want_text_meta[4] = {0xC2U, 0x80U, 0xA5U, 0xA5U};
    err |= expect_int("packet protocol two bytes", (int)m17_encode_packet_protocol_id(0x80U, out), 2);
    err |= expect_bytes("packet protocol two-byte layout", out, want_text_meta, sizeof(want_text_meta));

    DSD_MEMSET(out, 0xA5, sizeof(out));
    static const uint8_t want_three[4] = {0xE0U, 0xA0U, 0x80U, 0xA5U};
    err |= expect_int("packet protocol three bytes", (int)m17_encode_packet_protocol_id(0x0800U, out), 3);
    err |= expect_bytes("packet protocol three-byte layout", out, want_three, sizeof(want_three));

    DSD_MEMSET(out, 0xA5, sizeof(out));
    static const uint8_t want_max[4] = {0xF7U, 0xBFU, 0xBFU, 0xBFU};
    err |=
        expect_int("packet protocol four bytes", (int)m17_encode_packet_protocol_id(M17_PACKET_PROTOCOL_MAX, out), 4);
    err |= expect_bytes("packet protocol four-byte layout", out, want_max, sizeof(want_max));

    return err;
}

static int
test_m17_hook_argument_guards(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t bits[M17_LICH_BITS];
    uint8_t payload_bits[M17_STREAM_PAYLOAD_BITS];
    uint8_t processed_bits[M17_STREAM_PAYLOAD_BITS];
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t lsf_packed[M17_LSF_BYTES];
    uint8_t conn[11];
    uint8_t disc[10];
    uint8_t eotx[10];
    uint8_t reversed[208];
    uint8_t bert_bits[M17_BERT_PAYLOAD_BITS];
    int err = 0;

    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMSET(bits, 0, sizeof(bits));
    DSD_MEMSET(payload_bits, 0, sizeof(payload_bits));
    DSD_MEMSET(processed_bits, 0x5A, sizeof(processed_bits));
    DSD_MEMSET(lsf_bits, 0xA5, sizeof(lsf_bits));
    DSD_MEMSET(lsf_packed, 0x5A, sizeof(lsf_packed));
    DSD_MEMSET(conn, 0x11, sizeof(conn));
    DSD_MEMSET(disc, 0x22, sizeof(disc));
    DSD_MEMSET(eotx, 0x33, sizeof(eotx));
    DSD_MEMSET(reversed, 0x66, sizeof(reversed));
    DSD_MEMSET(bert_bits, 0, sizeof(bert_bits));

    err |= expect_int("process LICH rejects null state", m17_process_lich(NULL, opts, bits), -1);
    err |= expect_int("process LICH rejects null opts", m17_process_lich(state, NULL, bits), -1);
    err |= expect_int("process LICH rejects null bits", m17_process_lich(state, opts, NULL), -1);

    err |= expect_int("stream dispatch rejects null state",
                      m17_dispatch_stream_payload(opts, NULL, payload_bits, 0U, processed_bits), M17_STREAM_INVALID);
    err |= expect_int("stream dispatch rejects null payload",
                      m17_dispatch_stream_payload(opts, state, NULL, 0U, processed_bits), M17_STREAM_INVALID);
    err |= expect_int("stream dispatch rejects null output",
                      m17_dispatch_stream_payload(opts, state, payload_bits, 0U, NULL), M17_STREAM_INVALID);

    state->m17_bert_bits = 1234U;
    m17_process_bert_payload(NULL, state, bert_bits);
    err |= expect_int("BERT null opts preserves state", (int)state->m17_bert_bits, 1234);
    m17_process_bert_payload(opts, state, NULL);
    err |= expect_int("BERT null bits preserves state", (int)state->m17_bert_bits, 1234);

    state->carrier = 1;
    state->synctype = DSD_SYNC_M17_STR_POS;
    m17_ip_dispatch_frame(NULL, state, bits, sizeof(bits));
    err |= expect_int("IP dispatch null opts preserves carrier", state->carrier, 1);
    m17_ip_dispatch_frame(opts, NULL, bits, sizeof(bits));
    m17_ip_dispatch_frame(opts, state, NULL, sizeof(bits));
    err |= expect_int("IP dispatch null frame preserves synctype", state->synctype, DSD_SYNC_M17_STR_POS);

    m17_setup_conn_disc_eotx(0x0123456789ABULL, 'A', NULL, disc, eotx);
    err |= expect_u8("setup CONN null guard preserves DISC", disc[0], 0x22U);
    m17_setup_conn_disc_eotx(0x0123456789ABULL, 'A', conn, NULL, eotx);
    err |= expect_u8("setup DISC null guard preserves CONN", conn[0], 0x11U);
    m17_setup_conn_disc_eotx(0x0123456789ABULL, 'A', conn, disc, NULL);
    err |= expect_u8("setup EOTX null guard preserves EOTX", eotx[0], 0x33U);

    err |= expect_int("attach LSF CRC rejects null LSF", m17_attach_lsf_crc(NULL, lsf_packed), 0);
    err |= expect_int("attach LSF CRC rejects null packed", m17_attach_lsf_crc(lsf_bits, NULL), 0);
    m17_load_lsf_callsigns(NULL, 1ULL, 2ULL);
    m17_reverse_brt_bits(NULL, reversed);
    err |= expect_u8("reverse BRT null input preserves output", reversed[0], 0x66U);

    return err;
}

static int
test_encoders_propagate_requested_udp_setup_failure(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.m17_use_ip = 1;
    opts.audio_in_type = AUDIO_IN_PULSE;
    state.m17_rate = 48000;
    g_m17_connect_calls = 0;
    g_m17_connect_result = -1;
    dsd_m17_udp_hooks_set((dsd_m17_udp_hooks){
        .connect = fake_m17_connect_failure,
    });

    int err = 0;
    err |= expect_int("stream encoder reports requested UDP failure", encodeM17STR(&opts, &state), -1);
    err |= expect_int("packet encoder reports requested UDP failure", encodeM17PKT(&opts, &state), -1);
    err |= expect_int("requested UDP state is preserved", opts.m17_use_ip, 1);
    err |= expect_int("each encoder attempted UDP setup", g_m17_connect_calls, 2);
    g_m17_connect_result = 9;
    err |= expect_int("stream encoder reports positive UDP setup error", encodeM17STR(&opts, &state), -1);
    err |= expect_int("positive UDP setup error attempted once", g_m17_connect_calls, 3);
    dsd_m17_udp_hooks_set((dsd_m17_udp_hooks){0});
    return err;
}

static int
test_local_stream_lsf_publishes_canonical_identity(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t lsf_packed[M17_LSF_BYTES];
    dsd_state_ext_free_all(state);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMSET(lsf_bits, 0, sizeof(lsf_bits));
    DSD_MEMSET(lsf_packed, 0, sizeof(lsf_packed));

    const unsigned long long dst = m17_encode_b40_callsign(0ULL, M17_REF_LSF_DST_CSD);
    const unsigned long long src = m17_encode_b40_callsign(0ULL, M17_REF_LSF_SRC_CSD);
    const uint16_t type_word = m17_compose_frame_info(1U, 2U, 0U, 0U, 7U, 0U, 0U);
    build_lsf_bits(lsf_bits, dst, src, type_word, NULL);
    const uint16_t crc = m17_attach_lsf_crc(lsf_bits, lsf_packed);
    DSD_MEMCPY(state->m17_lsf, lsf_bits, sizeof(lsf_bits));

    state->m17encoder_tx = 1;
    state->synctype = DSD_SYNC_M17_LSF_POS;
    state->m17_can_en = -1;
    opts->monitor_input_audio = 0;

    int err = 0;
    err |= expect_int("local stream LSF CRC", m17_finalize_lsf_crc(opts, state, lsf_packed, crc), 0);
    dsd_call_snapshot call;
    err |= expect_int("local stream LSF publishes call", get_m17_call(state, &call), 1);
    err |= expect_int("local stream LSF call active", call.phase, DSD_CALL_PHASE_ACTIVE);
    err |= expect_u64("local stream LSF destination", call.ota_target_id, dst);
    err |= expect_u64("local stream LSF source", call.ota_source_id, src);
    err |= expect_int("local stream LSF CAN", call.service_options, 7);
    err |= expect_int("local stream LSF service confirmed", call.has_service_metadata, 1);
    err |= expect_int("local stream LSF crypto clear", call.crypto, DSD_CALL_CRYPTO_CLEAR);
    err |= expect_int("local stream LSF source text", strcmp(call.source_text, M17_REF_LSF_SRC_CSD), 0);
    err |= expect_int("local stream LSF destination text", strcmp(call.target_text, M17_REF_LSF_DST_CSD), 0);
    dsd_state_ext_free_all(state);
    DSD_MEMSET(state, 0, sizeof(*state));
    return err;
}

static int
test_monitored_stream_call_follows_tx_lifecycle(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    dsd_state_ext_free_all(state);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));

    const unsigned long long dst = m17_encode_b40_callsign(0ULL, M17_REF_LSF_DST_CSD);
    const unsigned long long src = m17_encode_b40_callsign(0ULL, M17_REF_LSF_SRC_CSD);
    uint64_t active_epoch = 0U;
    opts->monitor_input_audio = 1;

    m17_sync_monitored_tx_call(opts, state, dst, src, M17_REF_LSF_DST_CSD, M17_REF_LSF_SRC_CSD, 7U, &active_epoch);
    dsd_call_snapshot call;
    int err = 0;
    err |= expect_int("idle monitored encoder does not publish call", get_m17_call(state, &call), 0);

    state->m17encoder_tx = 1;
    m17_sync_monitored_tx_call(opts, state, dst, src, M17_REF_LSF_DST_CSD, M17_REF_LSF_SRC_CSD, 7U, &active_epoch);
    err |= expect_int("transmitting monitored encoder publishes call", get_m17_call(state, &call), 1);
    err |= expect_int("transmitting monitored call active", call.phase, DSD_CALL_PHASE_ACTIVE);
    err |= expect_u64("transmitting monitored call epoch tracked", active_epoch, call.epoch);
    err |= expect_u64("transmitting monitored destination", call.ota_target_id, dst);
    err |= expect_u64("transmitting monitored source", call.ota_source_id, src);
    err |= expect_int("transmitting monitored CAN", call.service_options, 7);
    err |= expect_int("transmitting monitored crypto clear", call.crypto, DSD_CALL_CRYPTO_CLEAR);
    err |= expect_int("transmitting monitored media active", call.media_active, 1);
    const uint64_t tx_epoch = call.epoch;

    state->m17encoder_tx = 0;
    m17_sync_monitored_tx_call(opts, state, dst, src, M17_REF_LSF_DST_CSD, M17_REF_LSF_SRC_CSD, 7U, &active_epoch);
    err |= expect_int("idle transition retains monitored call", get_m17_call(state, &call), 1);
    err |= expect_int("idle transition ends monitored call", call.phase, DSD_CALL_PHASE_ENDED);
    err |= expect_u64("idle transition preserves monitored epoch", call.epoch, tx_epoch);
    err |= expect_u64("idle transition clears tracked epoch", active_epoch, 0U);
    const uint64_t ended_revision = call.revision;

    m17_sync_monitored_tx_call(opts, state, dst, src, M17_REF_LSF_DST_CSD, M17_REF_LSF_SRC_CSD, 7U, &active_epoch);
    (void)get_m17_call(state, &call);
    err |= expect_u64("repeated monitored idle is idempotent", call.revision, ended_revision);

    dsd_state_ext_free_all(state);
    DSD_MEMSET(state, 0, sizeof(*state));
    return err;
}

static int
test_packet_encoder_monitors_lsf_with_canonical_viterbi(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t expected_lsf[TEST_M17_LSF_BITS];
    uint8_t expected_packed[M17_LSF_BYTES];
    static Event_History_I event_history[2];
    dsd_state_ext_free_all(state);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMSET(expected_lsf, 0, sizeof(expected_lsf));
    DSD_MEMSET(expected_packed, 0, sizeof(expected_packed));
    DSD_MEMSET(event_history, 0, sizeof(event_history));
    for (uint8_t slot = 0U; slot < 2U; slot++) {
        init_event_history(&event_history[slot], 0, 255);
    }
    state->event_history_s = event_history;

    state->m17_can_en = -1;
    DSD_SNPRINTF(state->m17sms, sizeof(state->m17sms), "%s", "OK");
    const unsigned long long dst = 0xFFFFFFFFFFFFULL;
    const unsigned long long src = m17_encode_b40_callsign(0ULL, "DSD-neo  ");
    m17_load_lsf_callsigns(expected_lsf, dst, src);
    write_bits_from_u64(expected_lsf + 96, m17_compose_frame_info(0U, 0U, 0U, 0U, 7U, 0U, 0U), 16U);
    (void)m17_attach_lsf_crc(expected_lsf, expected_packed);

    reset_convolution_fake();
    exitflag = 0;
    const int encode_rc = encodeM17PKT(opts, state);
    exitflag = 0;

    int err = 0;
    err |= expect_int("packet encoder completes", encode_rc, 0);
    err |= expect_int("packet LSF monitor bypasses NXDN decoder start", g_conv_start_calls, 0);
    err |= expect_int("packet LSF monitor bypasses NXDN decoder symbols", g_conv_decode_calls, 0);
    err |= expect_int("packet LSF monitor bypasses NXDN decoder chainback", g_conv_chainback_calls, 0);
    err |= expect_bytes("packet LSF monitor exact roundtrip", state->m17_lsf, expected_lsf, sizeof(expected_lsf));
    dsd_call_snapshot call;
    err |= expect_int("packet LSF monitor publishes identity", get_m17_call(state, &call), 1);
    err |= expect_int("packet LSF monitor publishes data kind", call.kind, DSD_CALL_KIND_DATA);
    err |= expect_int("packet encoder ends canonical call", call.phase, DSD_CALL_PHASE_ENDED);
    err |= expect_u64("packet LSF monitor destination", call.ota_target_id, dst);
    err |= expect_u64("packet LSF monitor source", call.ota_source_id, src);
    err |= expect_u8("packet LSF monitor CAN", state->m17_can, 7U);
    err |= expect_int("packet encoder clears current event",
                      event_history[0].Event_History_Items[0].event_string[0] == '\0', 1);
    err |= expect_int("packet encoder commits event history",
                      event_history[0].Event_History_Items[1].event_string[0] != '\0', 1);

    const uint64_t first_epoch = call.epoch;
    exitflag = 0;
    err |= expect_int("next matching packet encoder completes", encodeM17PKT(opts, state), 0);
    exitflag = 0;
    err |= expect_int("next matching packet encoder retains call", get_m17_call(state, &call), 1);
    err |= expect_int("next matching packet encoder ends call", call.phase, DSD_CALL_PHASE_ENDED);
    err |= expect_int("next matching packet encoder uses distinct epoch", call.epoch != first_epoch, 1);
    dsd_state_ext_free_all(state);
    DSD_MEMSET(state, 0, sizeof(*state));
    return err;
}

static int
open_empty_m17_wav_input(char* path, size_t path_size, SNDFILE** input) {
    if (path == NULL || path_size == 0U || input == NULL) {
        return -1;
    }
    *input = NULL;

    int fd = dsd_test_mkstemp(path, path_size, "m17_stdin_eof");
    if (fd < 0) {
        return -1;
    }
    if (dsd_close(fd) != 0) {
        (void)remove(path);
        return -1;
    }

    SF_INFO info = {0};
    info.samplerate = 8000;
    info.channels = 1;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    SNDFILE* output = sf_open(path, SFM_WRITE, &info);
    if (output == NULL) {
        (void)remove(path);
        return -1;
    }
    if (sf_close(output) != 0) {
        (void)remove(path);
        return -1;
    }

    DSD_MEMSET(&info, 0, sizeof(info));
    *input = sf_open(path, SFM_READ, &info);
    if (*input == NULL) {
        (void)remove(path);
        return -1;
    }
    return 0;
}

static int
test_stream_encoder_treats_stdin_eof_as_clean_shutdown(void) {
    static dsd_opts opts;
    static dsd_state state;
    char path[DSD_TEST_PATH_MAX] = {0};
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    if (open_empty_m17_wav_input(path, sizeof(path), &opts.audio_in_file) != 0) {
        DSD_FPRINTF(stderr, "failed to create empty M17 stdin fixture: %s\n", sf_strerror(NULL));
        return 1;
    }
    opts.audio_in_type = AUDIO_IN_STDIN;
    state.m17_can_en = -1;
    state.m17_rate = 8000;
    exitflag = 0;

    int err = 0;
    err |= expect_int("stream encoder treats stdin EOF as success", encodeM17STR(&opts, &state), 0);
    err |= expect_int("stream encoder closes exhausted stdin", opts.audio_in_file == NULL, 1);

    if (opts.audio_in_file != NULL) {
        (void)sf_close(opts.audio_in_file);
        opts.audio_in_file = NULL;
    }
    exitflag = 0;
    (void)remove(path);
    return err;
}

static int
test_ip_decoder_propagates_udp_backend_failure(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    dsd_m17_udp_hooks_set((dsd_m17_udp_hooks){
        .udp_bind = fake_m17_bind_failure,
    });

    int err = 0;
    err |= expect_int("IP decoder reports UDP bind failure", processM17IPF(&opts, &state), -1);

    state.event_history_s = calloc(2U, sizeof(*state.event_history_s));
    if (state.event_history_s == NULL) {
        dsd_m17_udp_hooks_set((dsd_m17_udp_hooks){0});
        return 1;
    }
    for (uint8_t slot = 0U; slot < 2U; slot++) {
        init_event_history(&state.event_history_s[slot], 0, 255);
    }

    g_m17_receiver_calls = 0;
    exitflag = 0;
    dsd_m17_udp_hooks_set((dsd_m17_udp_hooks){
        .udp_bind = fake_m17_bind_success,
        .receiver = fake_m17_receiver_idle_then_shutdown,
    });
    err |= expect_int("IP decoder keeps polling after idle receive", processM17IPF(&opts, &state), 0);
    err |= expect_int("IP decoder polls again after idle receive", g_m17_receiver_calls, 2);
    exitflag = 0;
    free(state.event_history_s);
    state.event_history_s = NULL;

    g_m17_receiver_calls = 0;
    dsd_m17_udp_hooks_set((dsd_m17_udp_hooks){
        .udp_bind = fake_m17_bind_success,
        .receiver = fake_m17_receiver_failure,
    });
    err |= expect_int("IP decoder reports UDP receive failure", processM17IPF(&opts, &state), -1);
    err |= expect_int("IP decoder stops after receive failure", g_m17_receiver_calls, 1);
    dsd_m17_udp_hooks_set((dsd_m17_udp_hooks){0});
    return err;
}

/* An M17 preamble is only an alternating symbol run, so the frame body has to prove itself
 * before the decoder publishes identity or plays audio (issue #399). */
static int
test_lsf_crc_confirms_the_transmission(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t lsf_packed[M17_LSF_BYTES];

    dsd_state_ext_free_all(state);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    const unsigned long long dst = m17_encode_b40_callsign(0ULL, M17_REF_LSF_DST_CSD);
    const unsigned long long src = m17_encode_b40_callsign(0ULL, M17_REF_LSF_SRC_CSD);
    const uint16_t type_word = m17_compose_frame_info(1U, 2U, 0U, 0U, 5U, 0U, 0U);
    build_lsf_bits(lsf_bits, dst, src, type_word, NULL);
    const uint16_t crc = m17_attach_lsf_crc(lsf_bits, lsf_packed);
    opts->aggressive_framesync = 1;

    int err = 0;
    err |= expect_int("a fresh transmission has proved nothing", m17_confirm_is_confirmed(state), 0);

    DSD_MEMCPY(state->m17_lsf, lsf_bits, sizeof(lsf_bits));
    err |= expect_int("a failing LSF CRC reports an error", m17_finalize_lsf_crc(opts, state, lsf_packed, crc ^ 1U), 1);
    err |= expect_int("a failing LSF CRC confirms nothing", m17_confirm_is_confirmed(state), 0);

    DSD_MEMCPY(state->m17_lsf, lsf_bits, sizeof(lsf_bits));
    err |= expect_int("a passing LSF CRC reports no error", m17_finalize_lsf_crc(opts, state, lsf_packed, crc), 0);
    err |= expect_int("a passing LSF CRC confirms the transmission", m17_confirm_is_confirmed(state), 1);

    m17_confirm_reset(state);
    err |= expect_int("reset forgets the transmission", m17_confirm_is_confirmed(state), 0);
    dsd_state_ext_free_all(state);
    return err;
}

static int
test_unconfirmed_stream_publishes_no_call(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t payload_bits[M17_STREAM_PAYLOAD_BITS];
    uint8_t processed_bits[M17_STREAM_PAYLOAD_BITS];
    dsd_call_snapshot call;

    dsd_state_ext_free_all(state);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMSET(payload_bits, 0, sizeof(payload_bits));
    state->synctype = DSD_SYNC_M17_STR_POS;
    state->m17_str_dt = 2U;
    state->m17_can_en = -1;

    /* A stream frame whose LICH happened to clear, on a transmission that has proved nothing:
     * this is what a false preamble chain looks like, and it must stay silent. */
    int err = 0;
    err |= expect_int("an unconfirmed stream still dispatches",
                      m17_dispatch_stream_payload(opts, state, payload_bits, M17_REF_STREAM_FN, processed_bits),
                      M17_STREAM_CLEAR_DISPATCHED);
    err |= expect_int("an unconfirmed stream publishes no call", get_m17_call(state, &call), 0);

    /* One clean LICH is not proof; two frames running are. */
    m17_confirm_begin_frame(state);
    m17_confirm_note_evidence(state, M17_EVIDENCE_WEAK);
    m17_confirm_end_frame(state);
    err |= expect_int("one clean LICH does not open a call",
                      m17_dispatch_stream_payload(opts, state, payload_bits, M17_REF_STREAM_FN, processed_bits),
                      M17_STREAM_CLEAR_DISPATCHED);
    err |= expect_int("one clean LICH publishes no call", get_m17_call(state, &call), 0);

    m17_confirm_begin_frame(state);
    m17_confirm_note_evidence(state, M17_EVIDENCE_WEAK);
    m17_confirm_end_frame(state);
    err |= expect_int("a confirmed stream dispatches",
                      m17_dispatch_stream_payload(opts, state, payload_bits, M17_REF_STREAM_FN, processed_bits),
                      M17_STREAM_CLEAR_DISPATCHED);
    err |= expect_int("a confirmed stream publishes a call", get_m17_call(state, &call), 1);
    err |= expect_u8("a confirmed stream marks media", call.media_active, 1U);
    dsd_state_ext_free_all(state);
    return err;
}

/* Issue #518: the stream encoder's microphone chain covers the whole codec2 frame, 320 samples at 1600 bit/s as well as
   160 at 3200, so the second half of a 1600 frame is filtered and gained like the first and the AGC counts time in the
   samples it actually ran. A steady 1 kHz tone 20 dB under the PCM reference settles at the AGC's boost limit (18 dB
   over unity) in both halves of the last frame, at either length. */
static int
voice_chain_settled_peaks(size_t nsam, double* first_half, double* second_half) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    opts.use_pbf = 1;
    opts.audio_gainA = 0.0f;
    m17_voice_chain chain;
    m17_voice_chain_init(&chain);
    short voice[320];
    double phase = 0.0;
    const size_t frames = (size_t)(3 * 8000) / nsam;
    for (size_t f = 0; f < frames; f++) {
        for (size_t i = 0; i < nsam; i++) {
            voice[i] = (short)lrint(823.1 * sin(phase));
            phase += 2.0 * 3.14159265358979323846 * 1000.0 / 8000.0;
        }
        m17_voice_chain_process(&opts, &state, &chain, voice, nsam);
    }
    *first_half = 0.0;
    *second_half = 0.0;
    for (size_t i = 0; i < nsam; i++) {
        const double a = fabs((double)voice[i]);
        double* half = i < nsam / 2 ? first_half : second_half;
        *half = a > *half ? a : *half;
    }
    return 0;
}

static int
test_stream_voice_chain_covers_the_whole_frame(void) {
    int err = 0;
    const size_t lengths[] = {160U, 320U};
    const double want = 823.1 * pow(10.0, DSD_VOICE_AGC_MAX_BOOST_DB / 20.0);
    for (size_t l = 0; l < sizeof lengths / sizeof lengths[0]; l++) {
        double first = 0.0;
        double second = 0.0;
        (void)voice_chain_settled_peaks(lengths[l], &first, &second);
        const int ok = fabs(20.0 * log10(first / want)) < 0.5 && fabs(20.0 * log10(second / want)) < 0.5;
        if (!ok) {
            DSD_FPRINTF(stderr, "voice chain at %zu samples: halves peak %.1f / %.1f, want %.1f\n", lengths[l], first,
                        second, want);
        }
        err |= expect_int("voice chain covers the whole frame", ok, 1);
    }
    return err;
}

/* The RMS of @p n samples as a sine amplitude, in dB over @p ref. */
static double
amplitude_db(const short* x, size_t n, double ref) {
    double acc = 0.0;
    for (size_t i = 0; i < n; i++) {
        acc += (double)x[i] * (double)x[i];
    }
    return 20.0 * log10(sqrt(2.0 * acc / (double)n) / ref);
}

/* The steady-state output level, in dB, of a @p hz tone through the voice chain at 8 kHz with only the filter flags
   given set and a fixed gain of exactly 1x (-n 20). */
static double
voice_chain_tone_db(int use_lpf, int use_hpf, double hz) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    /* What production leaves in dsd_state: its filters designed for the monitor's 48 kHz input. */
    init_audio_filters(&state, 48000);
    opts.use_lpf = use_lpf;
    opts.use_hpf = use_hpf;
    opts.audio_gainA = 20.0f;
    m17_voice_chain chain;
    m17_voice_chain_init(&chain);
    short voice[160];
    double phase = 0.0;
    for (int f = 0; f < 50; f++) {
        for (size_t i = 0; i < 160U; i++) {
            voice[i] = (short)lrint(8000.0 * sin(phase));
            phase += 2.0 * 3.14159265358979323846 * hz / 8000.0;
        }
        m17_voice_chain_process(&opts, &state, &chain, voice, 160U);
    }
    return amplitude_db(voice, 160U, 8000.0);
}

/* The 960 Hz RC one-pole's response at @p hz on 8 kHz samples, in dB. */
static double
onepole_db(int highpass, double hz) {
    const double pi = 3.14159265358979323846;
    const double ts = 1.0 / 8000.0;
    const double rc = 1.0 / (2.0 * pi * 960.0);
    const double w = 2.0 * pi * hz / 8000.0;
    const double p = rc / (ts + rc);
    const double den = sqrt(1.0 - (2.0 * p * cos(w)) + (p * p));
    const double num = highpass ? p * 2.0 * sin(w / 2.0) : 1.0 - p;
    return 20.0 * log10(num / den);
}

/* Issue #617: the encoder's -v 0x2 / 0x4 filters are its own, designed at codec2's 8 kHz. Run from dsd_state's, designed
   for the 48 kHz input, both acted at 160 Hz: the low-pass took 1 kHz about 16 dB down and the high-pass passed 500 Hz
   nearly whole. */
static int
test_stream_voice_chain_onepoles_run_at_8k(void) {
    const double lp = voice_chain_tone_db(1, 0, 1000.0);
    const double hp = voice_chain_tone_db(0, 1, 500.0);
    const double flat = voice_chain_tone_db(0, 0, 1000.0);
    const int ok = fabs(lp - onepole_db(0, 1000.0)) < 0.2 && fabs(hp - onepole_db(1, 500.0)) < 0.2 && fabs(flat) < 0.05;
    if (!ok) {
        DSD_FPRINTF(stderr,
                    "voice chain one-poles: lp 1 kHz %.2f dB (want %.2f), hp 500 Hz %.2f dB (want %.2f), flat %.2f\n",
                    lp, onepole_db(0, 1000.0), hp, onepole_db(1, 500.0), flat);
    }
    return expect_int("voice chain one-poles run at 8 kHz", ok, 1);
}

/* A tone the fake inputs below deliver, one sample per read at g_tone_rate_hz. */
static double g_tone_hz;
static double g_tone_amplitude;
static double g_tone_phase;
static int g_tone_rate_hz;

static double
next_tone_sample(void) {
    const double v = g_tone_amplitude * sin(g_tone_phase);
    g_tone_phase += 2.0 * 3.14159265358979323846 * g_tone_hz / (double)g_tone_rate_hz;
    return v;
}

static void
start_tone(double hz, double amplitude, int rate_hz) {
    g_tone_hz = hz;
    g_tone_amplitude = amplitude;
    g_tone_phase = 0.0;
    g_tone_rate_hz = rate_hz;
}

static int
fake_udp_tone_read(dsd_opts* opts, int16_t* out) {
    (void)opts;
    if (out == NULL) {
        return 0;
    }
    *out = (int16_t)lrint(next_tone_sample());
    return 1;
}

/* The last frame encoder_read_db() read, for the saturation checks. */
static short g_encoder_frame[160];

/* Read 10 codec2 frames through the encoder's own input setup (m17_encoder_input_init() at @p rate_hz) and reader, and
   return the last frame's level in dB over @p ref, with its peak in @p peak. */
static double
encoder_read_db(dsd_opts* opts, dsd_state* state, int rate_hz, double ref, double* peak) {
    m17_encoder_input in;
    m17_encoder_input_init(&in, rate_hz);
    short out[160];
    int ok = 1;
    for (int b = 0; b < 10; b++) {
        ok &= m17_encoder_read_block(opts, state, &in, out, 160U) == 1;
    }
    *peak = 0.0;
    for (size_t i = 0; i < 160U; i++) {
        const double a = fabs((double)out[i]);
        *peak = a > *peak ? a : *peak;
    }
    DSD_MEMCPY(g_encoder_frame, out, sizeof g_encoder_frame);
    return ok ? amplitude_db(out, 160U, ref) : 999.0;
}

static double
encoder_udp_tone_db(double hz, double amplitude, int rate_hz, int volume, double* peak) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    opts.audio_in_type = AUDIO_IN_UDP;
    opts.input_volume_multiplier = volume;
    start_tone(hz, amplitude, rate_hz);
    dsd_net_audio_input_hooks_set((dsd_net_audio_input_hooks){.udp_read_sample = fake_udp_tone_read});
    const double level = encoder_read_db(&opts, &state, rate_hz, amplitude, peak);
    dsd_net_audio_input_hooks_set((dsd_net_audio_input_hooks){0});
    return level;
}

static int g_silent_pending = 0;
static int g_silent_waits = 0;
static int g_silent_spurious_first = 0;       /* the first wait comes back at once, empty, as a spurious wakeup can */
static uint64_t g_silent_first_empty_ms = 0U; /* when the first wait came back empty */

static int
fake_controls_pending(void) {
    return g_silent_pending;
}

static int
fake_udp_silent_wait(dsd_opts* opts, int16_t* out, unsigned int timeout_ms) {
    (void)opts;
    (void)out;
    g_silent_waits++;
    if (g_silent_waits > 1 || !g_silent_spurious_first) {
        dsd_sleep_ms(timeout_ms);
    }
    if (g_silent_waits == 1) {
        g_silent_first_empty_ms = dsd_realtime_mono_ms();
    }
    return -1;
}

/* Issue #634: the stream encoder reads its UDP input a block at a time and applies queued commands between blocks. A
   silent input whose silence lasts a stream pause gives the block up for a queued command (2, nothing read), so the
   encoder applies it and reads the block again, and encodes nothing of the block it gave up. */
static int
test_encoder_silent_udp_gives_way_to_a_queued_command(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    opts.audio_in_type = AUDIO_IN_UDP;
    opts.input_volume_multiplier = 1;
    m17_encoder_input in;
    m17_encoder_input_init(&in, 8000);
    dsd_net_audio_input_hooks hooks;
    DSD_MEMSET(&hooks, 0, sizeof hooks);
    hooks.udp_read_sample_wait = fake_udp_silent_wait;
    dsd_net_audio_input_hooks_set(hooks);
    dsd_runtime_set_controls_pending(fake_controls_pending);
    g_silent_pending = 1;
    g_silent_waits = 0;
    short out[160];
    int rc = 0;
    if (m17_encoder_read_block(&opts, &state, &in, out, 160U) != 2) {
        DSD_FPRINTF(stderr, "silent udp: the block was not given up for the queued command\n");
        rc = 1;
    }
    /* Not before the silence lasted a stream pause, counted from the first wait that came back empty. It is timed, not
       counted in waits: a loaded host's 100 ms sleeps run long, and fewer of them make up the pause. */
    const uint64_t silent_ms = g_silent_waits > 1 ? dsd_realtime_mono_ms() - g_silent_first_empty_ms : 0U;
    if (silent_ms < DSD_ANALOG_STREAM_PAUSE_MIN_MS) {
        DSD_FPRINTF(stderr, "silent udp: gave up after %d waits, %llu ms of silence\n", g_silent_waits,
                    (unsigned long long)silent_ms);
        rc = 1;
    }
    /* A first wait that came back at once (a spurious wakeup) is no silence: the pause is still whole. */
    g_silent_waits = 0;
    g_silent_spurious_first = 1;
    const uint64_t start_ms = dsd_realtime_mono_ms();
    if (m17_encoder_read_block(&opts, &state, &in, out, 160U) != 2) {
        DSD_FPRINTF(stderr, "silent udp after a spurious wakeup: the block was not given up\n");
        rc = 1;
    }
    const uint64_t waited_ms = dsd_realtime_mono_ms() - start_ms;
    if (waited_ms < DSD_ANALOG_STREAM_PAUSE_MIN_MS) {
        DSD_FPRINTF(stderr, "silent udp after a spurious wakeup: gave up after %llu ms\n",
                    (unsigned long long)waited_ms);
        rc = 1;
    }
    g_silent_spurious_first = 0;
    g_silent_pending = 0;
    dsd_runtime_set_controls_pending(NULL);
    dsd_net_audio_input_hooks_set((dsd_net_audio_input_hooks){0});
    return rc;
}

/* Issue #618: every encoder input keeps one sample in INPUT_RATE / 8000, low-passed first. A PCM microphone at 48 kHz
   passes 1 kHz whole and takes a 7 kHz tone, which used to fold onto 1 kHz at full level, more than 20 dB down; at
   8 kHz nothing is filtered; a hot input saturates at full scale instead of wrapping. */
static int
test_encoder_pcm_input_is_anti_aliased(void) {
    double peak = 0.0;
    const double passed = encoder_udp_tone_db(1000.0, 8000.0, 48000, 1, &peak);
    const double aliased = encoder_udp_tone_db(7000.0, 8000.0, 48000, 1, &peak);
    const double native = encoder_udp_tone_db(1000.0, 8000.0, 8000, 1, &peak);
    double hot_peak = 0.0;
    (void)encoder_udp_tone_db(1000.0, 8000.0, 48000, 8, &hot_peak);
    const int ok = fabs(passed) < 0.2 && aliased < -20.0 && fabs(native) < 0.05 && hot_peak >= 32767.0;
    if (!ok) {
        DSD_FPRINTF(stderr,
                    "encoder PCM input: 1 kHz %.2f dB, 7 kHz alias %.2f dB, 8 kHz input %.2f dB, hot peak %.0f\n",
                    passed, aliased, native, hot_peak);
    }
    return expect_int("encoder PCM input anti-aliased", ok, 1);
}

#ifdef USE_RADIO
static int
fake_rtl_tone_read(void* rtl_ctx, float* out, size_t count, int* out_got) {
    (void)rtl_ctx;
    if (out == NULL || out_got == NULL || count != 1U) {
        return -1;
    }
    out[0] = (float)next_tone_sample();
    *out_got = 1;
    return 0;
}

static double
fake_rtl_return_pwr(const void* rtl_ctx) {
    (void)rtl_ctx;
    return 0.0;
}

/* The level, in dB over the -12 dBFS reference peak, of a @p hz tone the RTL monitor delivers at 48 kHz with
   @p amplitude, after the encoder's RTL input took it to 8 kHz at vol @p volume. */
static double
encoder_rtl_tone_db(double hz, double amplitude, int volume) {
    static dsd_opts opts;
    static dsd_state state;
    static int token;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtl_volume_multiplier = volume;
    state.rtl_ctx = (struct RtlSdrContext*)&token;
    start_tone(hz, amplitude, 48000);
    dsd_rtl_stream_io_hooks_set((dsd_rtl_stream_io_hooks){
        .read = fake_rtl_tone_read,
        .return_pwr = fake_rtl_return_pwr,
    });
    double peak = 0.0;
    const double level = encoder_read_db(&opts, &state, 48000, 8231.0, &peak);
    dsd_rtl_stream_io_hooks_set((dsd_rtl_stream_io_hooks){0});
    return level;
}

/* Issue #618: the encoder's RTL input reaches codec2 at PCM scale. The monitor's normalised samples (1 kHz at 3 kHz
   deviation reads 0.125, 0.25 after the default vol 2) were converted to short as they stood, so every one rounded to 0
   and codec2 encoded silence; the RTL monitor gain now takes that reference to -12 dBFS peak. The RTL input is
   low-passed before one sample in six is kept too. */
static int
test_encoder_rtl_input_reaches_pcm_scale(void) {
    const double reference = encoder_rtl_tone_db(1000.0, 0.125, 2);
    const double aliased = encoder_rtl_tone_db(7000.0, 0.125, 2);
    /* Full deviation at the top vol trim takes the reference far past int16 (0.5 x 3 x 32924): only the reader's
       saturating conversion keeps it there, at full scale in both polarities, where a plain cast would wrap. */
    (void)encoder_rtl_tone_db(1000.0, 0.5, 3);
    short lo = 0;
    short hi = 0;
    for (size_t i = 0; i < 160U; i++) {
        lo = g_encoder_frame[i] < lo ? g_encoder_frame[i] : lo;
        hi = g_encoder_frame[i] > hi ? g_encoder_frame[i] : hi;
    }
    const int ok = fabs(reference) < 0.2 && aliased < -20.0 && hi == 32767 && lo == -32768;
    if (!ok) {
        DSD_FPRINTF(stderr, "encoder RTL input: 1 kHz reference %.2f dB, 7 kHz alias %.2f dB, hot %d..%d\n", reference,
                    aliased, lo, hi);
    }
    return expect_int("encoder RTL input reaches PCM scale, anti-aliased, saturating", ok, 1);
}
#endif

#ifdef DSD_NEO_TEST_AUDIO_WRAP
/* The local output's writes ('W', 'Z' for all zeros) and drains ('D') in order, while g_audio_wrap_on. */
static char g_audio_events[2048];
static int g_audio_event_count = 0;
static int g_audio_wrap_on = 0;

// GNU linker wrappers of the local output (-Wl,--wrap): the names are the linker's.
// NOLINTBEGIN(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
int __real_dsd_audio_write(dsd_audio_stream* stream, const int16_t* buffer, size_t frames);
int __real_dsd_audio_drain(dsd_audio_stream* stream);
int __wrap_dsd_audio_write(dsd_audio_stream* stream, const int16_t* buffer, size_t frames);
int __wrap_dsd_audio_drain(dsd_audio_stream* stream);

int
__wrap_dsd_audio_write(dsd_audio_stream* stream, const int16_t* buffer, size_t frames) {
    if (!g_audio_wrap_on) {
        return __real_dsd_audio_write(stream, buffer, frames);
    }
    int zero = 1;
    for (size_t i = 0U; buffer != NULL && i < frames; i++) {
        zero &= buffer[i] == 0;
    }
    if (g_audio_event_count < (int)sizeof(g_audio_events) - 1) {
        g_audio_events[g_audio_event_count++] = zero ? 'Z' : 'W';
        g_audio_events[g_audio_event_count] = '\0';
    }
    return (int)frames;
}

int
__wrap_dsd_audio_drain(dsd_audio_stream* stream) {
    if (!g_audio_wrap_on) {
        return __real_dsd_audio_drain(stream);
    }
    if (g_audio_event_count < (int)sizeof(g_audio_events) - 1) {
        g_audio_events[g_audio_event_count++] = 'D';
        g_audio_events[g_audio_event_count] = '\0';
    }
    return 0;
}

// NOLINTEND(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)

/* Issue #625: the packet encoder's end drains a local output after its EOT marker, before its dead air, and again
   after the dead air, before it returns: an asynchronous output drops its oldest samples when full, and the engine
   closes it without a drain. */
static int
test_packet_encoder_end_drains_on_a_local_output(void) {
    int err = 0;
    static dsd_opts popts;
    static dsd_state pstate;
    static int ptoken;
    static Event_History_I pevents[2];
    DSD_MEMSET(&popts, 0, sizeof popts);
    DSD_MEMSET(&pstate, 0, sizeof pstate);
    DSD_MEMSET(pevents, 0, sizeof pevents);
    for (uint8_t slot = 0U; slot < 2U; slot++) {
        init_event_history(&pevents[slot], 0, 255);
    }
    pstate.event_history_s = pevents;
    pstate.m17_can_en = -1;
    DSD_SNPRINTF(pstate.m17sms, sizeof(pstate.m17sms), "%s", "OK");
    popts.monitor_input_audio = 1;
    popts.audio_out = 1;
    popts.audio_out_type = 0;
    popts.audio_raw_out = (dsd_audio_stream*)&ptoken;
    g_audio_event_count = 0;
    g_audio_events[0] = '\0';
    g_audio_wrap_on = 1;
    exitflag = 0;
    (void)encodeM17PKT(&popts, &pstate);
    exitflag = 0;
    g_audio_wrap_on = 0;
    popts.audio_raw_out = NULL;
    static const char want_tail[] = "WWDZZZZZZZZZZZZZZZZZZZZZZZZZD";
    const size_t len = strlen(g_audio_events);
    const int ok =
        len >= sizeof want_tail - 1U && strcmp(g_audio_events + (len - (sizeof want_tail - 1U)), want_tail) == 0;
    if (!ok) {
        DSD_FPRINTF(stderr, "packet encoder local output events: %s\n", g_audio_events);
    }
    err |= expect_int("packet encoder: the end drains around the dead air on a local output", ok, 1);
    dsd_state_ext_free_all(&pstate);
    return err;
}
#endif

#ifdef USE_CODEC2
/* Issue #574: the Monitor's mute sets audio_out to 0 and leaves the output open, and the mixers then write nothing to
   any output type (dsd_output_*_block()). Decoded Codec2 voice follows the same rule on the local stream, UDP and the
   raw fd, for the 3200 payload (two frames) and the 1600 payload (one frame). */
static int
test_stream_voice_audio_honors_mute_on_every_output(void) {
    static const struct {
        const char* tag;
        uint8_t dt;
        int blocks;
        size_t block_bytes;
    } formats[] = {
        {"3200", 2U, 2, 160U * sizeof(short)},
        {"1600", 3U, 1, 320U * sizeof(short)},
    };

    static const int out_types[] = {
#ifdef DSD_NEO_TEST_AUDIO_WRAP
        0,
#endif
        1,
        8,
    };
    /* A 1600 payload carries arbitrary data in its second half; zeros there keep it out of the packet printer. */
    static const uint8_t payload_bytes[16] = {0x60U, 0x61U, 0x62U, 0x63U, 0x64U, 0x65U, 0x66U, 0x67U,
                                              0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U};
    uint8_t payload_bits[128];
    uint8_t processed_bits[128];
    bytes_to_bits(payload_bytes, payload_bits, sizeof(payload_bytes));

    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    int err = 0;
    for (size_t f = 0U; f < sizeof(formats) / sizeof(formats[0]); f++) {
        for (size_t o = 0U; o < sizeof(out_types) / sizeof(out_types[0]); o++) {
            for (int audio_out = 1; audio_out >= 0; audio_out--) {
                const int out_type = out_types[o];
                DSD_MEMSET(opts, 0, sizeof(*opts));
                DSD_MEMSET(state, 0, sizeof(*state));
                reset_audio_fakes();
                install_fake_udp_audio();
                opts->slot1_on = 1;
                opts->audio_out = audio_out;
                opts->audio_out_type = out_type;
                state->m17_str_dt = formats[f].dt;
                state->m17_can_en = -1;
                m17_confirm_note_evidence(state, M17_EVIDENCE_STRONG);

                char path[DSD_TEST_PATH_MAX];
                int fd = -1;
                if (out_type == 1) {
                    fd = dsd_test_mkstemp(path, sizeof(path), "dsdneo_m17_mute");
                    if (fd < 0) {
                        err |= expect_int("m17 mute: raw output temp file", fd >= 0, 1);
                        dsd_state_ext_free_all(state);
                        continue;
                    }
                    opts->audio_out_fd = fd;
                }
#ifdef DSD_NEO_TEST_AUDIO_WRAP
                g_audio_event_count = 0;
                g_audio_events[0] = '\0';
                g_audio_wrap_on = out_type == 0;
#endif

                (void)m17_dispatch_stream_payload(opts, state, payload_bits, M17_REF_STREAM_FN, processed_bits);

                int written = 0;
                if (out_type == 8) {
                    written = g_udp_audio_calls;
                } else if (out_type == 1) {
                    dsd_stat_t st;
                    DSD_MEMSET(&st, 0, sizeof(st));
                    if (dsd_fstat(fd, &st) == 0) {
                        written = (int)((size_t)st.st_size / formats[f].block_bytes);
                    }
                    (void)dsd_close(fd);
                    (void)remove(path);
                }
#ifdef DSD_NEO_TEST_AUDIO_WRAP
                if (out_type == 0) {
                    written = g_audio_event_count;
                }
                g_audio_wrap_on = 0;
#endif
                char label[96];
                DSD_SNPRINTF(label, sizeof(label), "m17 %s voice blocks on output %d with audio_out %d", formats[f].tag,
                             out_type, audio_out);
                err |= expect_int(label, written, audio_out ? formats[f].blocks : 0);
                reset_audio_fakes();
                dsd_state_ext_free_all(state);
            }
        }
    }
    return err;
}
#endif

/* Issue #625: the VOX's two decisions. A read is heard on the stream's gate while one runs, on the level squelch
   otherwise; past 10 closed reads, at a LICH superframe boundary, the transmitter unkeys. */
static int
test_encoder_vox_helpers(void) {
    int err = 0;
    int sql_hit = 11;
    err |= expect_int("a heard read keys", m17_encoder_vox_keyed(1, 3, &sql_hit), 1);
    err |= expect_int("a heard read resets the count", sql_hit, 0);
    for (int i = 0; i < 10; i++) {
        (void)m17_encoder_vox_keyed(0, 0, &sql_hit);
    }
    err |= expect_int("ten closed reads still keyed", m17_encoder_vox_keyed(0, 2, &sql_hit), 1);
    err |= expect_int("eleven closed reads, off the LICH boundary, still keyed", sql_hit, 11);
    err |= expect_int("eleven closed reads at the LICH boundary unkey", m17_encoder_vox_keyed(0, 0, &sql_hit), 0);
    err |= expect_int("no counter: keyed", m17_encoder_vox_keyed(0, 0, NULL), 1);

    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof opts);
    opts.rtl_squelch_mode = DSD_SQUELCH_MODE_AUTO;
    opts.rtl_pwr = 1.0;
    err |= expect_int("under a running gate a closed read is closed", m17_encoder_squelch_heard(&opts, 1, 0), 0);
    err |= expect_int("under a running gate an open sample is heard", m17_encoder_squelch_heard(&opts, 1, 1), 1);
    opts.rtl_squelch_mode = DSD_SQUELCH_MODE_LEVEL;
    opts.rtl_squelch_level = dB_to_pwr(-60.0);
    opts.rtl_pwr = dB_to_pwr(-90.0);
    err |= expect_int("without a gate the level decides: closed", m17_encoder_squelch_heard(&opts, 0, 1), 0);
    opts.rtl_pwr = dB_to_pwr(-30.0);
    err |= expect_int("without a gate the level decides: open", m17_encoder_squelch_heard(&opts, 0, 0), 1);
    return err;
}

#ifdef USE_RADIO
/* Issue #625: the encoder's RTL reader under a running gate (AUTO, the stream's status active with a plan). The read is
   heard while a sample is open, a closed read is not, and a closed stretch fades to exact silence; under LEVEL the
   flags are not read and the samples are the plain reader's. */
static long g_gate_reads = 0;
static long g_gate_open_until = 0;
static int g_gate_status_active = 1;

static int
fake_rtl_gate_read_ex(void* rtl_ctx, float* out, uint8_t* flags, size_t count, int* out_got) {
    (void)rtl_ctx;
    if (out == NULL || out_got == NULL || count != 1U) {
        return -1;
    }
    out[0] = (float)next_tone_sample();
    if (flags != NULL) {
        flags[0] = g_gate_reads < g_gate_open_until ? 0U : (uint8_t)DSD_SQUELCH_FLAG_CLOSED;
    }
    g_gate_reads++;
    *out_got = 1;
    return 0;
}

static int
fake_rtl_gate_read(void* rtl_ctx, float* out, size_t count, int* out_got) {
    return fake_rtl_gate_read_ex(rtl_ctx, out, NULL, count, out_got);
}

static int
fake_rtl_gate_status(const void* rtl_ctx, dsd_rtl_squelch_status* out) {
    (void)rtl_ctx;
    DSD_MEMSET(out, 0, sizeof(*out));
    out->active = g_gate_status_active;
    out->plan_valid = 1;
    return 0;
}

static double
fake_rtl_gate_return_pwr(const void* rtl_ctx) {
    (void)rtl_ctx;
    return 1.0;
}

static void
install_gate_rtl_hooks(void) {
    dsd_rtl_stream_io_hooks_set((dsd_rtl_stream_io_hooks){
        .read = fake_rtl_gate_read,
        .return_pwr = fake_rtl_gate_return_pwr,
        .read_ex = fake_rtl_gate_read_ex,
        .squelch_status = fake_rtl_gate_status,
    });
}

static int
test_encoder_rtl_reader_gates_closed_samples(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int token;
    int err = 0;
    for (int level = 0; level < 2; level++) {
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        opts.audio_in_type = AUDIO_IN_RTL;
        opts.rtl_volume_multiplier = 2;
        opts.rtl_squelch_mode = level ? DSD_SQUELCH_MODE_LEVEL : DSD_SQUELCH_MODE_AUTO;
        state.rtl_ctx = (struct RtlSdrContext*)&token;
        install_gate_rtl_hooks();
        start_tone(1000.0, 0.125, 8000);
        g_gate_reads = 0;
        g_gate_open_until = 400;
        m17_encoder_input in;
        m17_encoder_input_init(&in, 8000);
        short blocks[4][160];
        int heard[4] = {0};
        int gated[4] = {0};
        for (int b = 0; b < 4; b++) {
            if (m17_encoder_read_block(&opts, &state, &in, blocks[b], 160U) != 1) {
                err |= expect_int("encoder gate read", 0, 1);
            }
            heard[b] = in.squelch_heard;
            gated[b] = in.squelch_gated;
        }
        int peak[4] = {0};
        for (int b = 0; b < 4; b++) {
            for (int i = 0; i < 160; i++) {
                const int v = blocks[b][i] < 0 ? -blocks[b][i] : blocks[b][i];
                peak[b] = v > peak[b] ? v : peak[b];
            }
        }
        if (level) {
            /* LEVEL: no gate, every read heard, the tone plays on (the plain reader's samples). */
            err |= expect_int("level: no gate", gated[0] | gated[3], 0);
            err |= expect_int("level: every read heard", heard[0] & heard[3], 1);
            err |= expect_int("level: the tone plays past the flags", peak[3] > 1000, 1);
        } else {
            err |= expect_int("auto: the gate runs", gated[0] & gated[3], 1);
            err |= expect_int("auto: open reads heard", heard[0] & heard[1] & heard[2], 1);
            err |= expect_int("auto: a closed read is not heard", heard[3], 0);
            err |= expect_int("auto: open audio plays", peak[0] > 1000, 1);
            err |= expect_int("auto: closed audio is silence", peak[3], 0);
        }
    }
    g_gate_status_active = 0;
    DSD_MEMSET(&opts, 0, sizeof opts);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtl_squelch_mode = DSD_SQUELCH_MODE_AUTO;
    state.rtl_ctx = (struct RtlSdrContext*)&token;
    g_gate_reads = 0;
    g_gate_open_until = 0;
    m17_encoder_input in;
    m17_encoder_input_init(&in, 8000);
    short block[160];
    (void)m17_encoder_read_block(&opts, &state, &in, block, 160U);
    err |= expect_int("no gate running: the flags are not read", in.squelch_gated == 0 && in.squelch_heard == 1, 1);
    g_gate_status_active = 1;
    dsd_rtl_stream_io_hooks_set((dsd_rtl_stream_io_hooks){0});
    return err;
}

/* An encoder run on the fake RTL stream: the tone, its flags open for g_vox_open_reads samples, the transmitter's
   state recorded at the start of each 40 ms iteration (320 samples at 8 kHz, 3200 bit/s), and what it sent: the RF
   frames as the symbol capture records them, and the IP frames. */
enum { VOX_ITER_SAMPLES = 320, VOX_MAX_ITERS = 64 };

static dsd_state* g_vox_state = NULL;
static long g_vox_reads = 0;
static long g_vox_open_reads = 0;
static long g_vox_cap = 0;
static long g_vox_low_power_from = -1;
static long g_vox_exit_at = -1;
static long g_vox_manual_unkey_at = -1;
static int g_vox_tx[VOX_MAX_ITERS];
static int g_vox_ip_stream = 0;
static int g_vox_ip_eos = 0;
static int g_vox_ip_eotx = 0;
/* The monitored call's epoch as each IP stream frame went out (0 with no active call). */
static uint64_t g_vox_ip_epoch[VOX_MAX_ITERS];

/* RF as the symbol capture records it (opts->symbol_out_f): each frame's kind and, for a stream frame, its EOS bit. */
enum { VOX_RF_MAX = 512 };

static char g_vox_rf_kind[VOX_RF_MAX];
static uint8_t g_vox_rf_eos[VOX_RF_MAX];
static int g_vox_rf_count = 0;
/* Play the RF on a local output (audio_raw_out) instead of the UDP analog output: the write and drain order is
   recorded where the audio wraps are linked. */
static int g_vox_local_audio = 0;

/* Classify one 192-symbol RF frame: a preamble, an LSF, a stream frame (with its EOS bit: the first two punctured bits
   after the LICH are the convolution of the frame number's MSB from a zero state), the EOT marker, or dead air. */
static char
vox_rf_classify(const uint8_t* dibits, uint8_t* eos) {
    uint8_t ref[M17_FRAME_SYMBOLS];
    int all_three = 1;
    for (int i = 0; i < M17_FRAME_SYMBOLS; i++) {
        all_three &= dibits[i] == 3U;
    }
    if (all_three) {
        return 'D';
    }
    m17_fill_repeating_16bit_dibits(M17_EOT_MARKER_WORD, ref);
    if (memcmp(ref, dibits, sizeof ref) == 0) {
        return 'E';
    }
    m17_fill_repeating_16bit_dibits(M17_PREAMBLE_LSF_WORD, ref);
    if (memcmp(ref, dibits, sizeof ref) == 0) {
        return 'P';
    }
    m17_fill_sync_dibits_from_word(M17_SYNC_LSF_WORD, ref);
    if (memcmp(ref, dibits, M17_SYNC_SYMBOLS) == 0) {
        return 'L';
    }
    m17_fill_sync_dibits_from_word(M17_SYNC_STREAM_WORD, ref);
    if (memcmp(ref, dibits, M17_SYNC_SYMBOLS) != 0) {
        return '?';
    }
    uint8_t randomized[M17_PAYLOAD_BITS];
    uint8_t decoded[M17_PAYLOAD_BITS];
    for (int i = 0; i < M17_PAYLOAD_SYMBOLS; i++) {
        randomized[i * 2] = (uint8_t)((dibits[M17_SYNC_SYMBOLS + i] >> 1U) & 1U);
        randomized[(i * 2) + 1] = (uint8_t)(dibits[M17_SYNC_SYMBOLS + i] & 1U);
    }
    m17_payload_decode_bits(randomized, decoded);
    if (decoded[M17_LICH_BITS] != decoded[M17_LICH_BITS + 1]) {
        return '?';
    }
    *eos = decoded[M17_LICH_BITS];
    return 'S';
}

static void
vox_rf_parse(FILE* f) {
    g_vox_rf_count = 0;
    if (!f || fflush(f) != 0 || fseek(f, 0L, SEEK_SET) != 0) {
        return;
    }
    uint8_t dibits[M17_FRAME_SYMBOLS];
    unsigned char record[DSD_SYMBOL_CAPTURE_SOFT_RECORD_SIZE];
    int n = 0;
    while (fread(record, 1, sizeof record, f) == sizeof record) {
        dibits[n++] = (uint8_t)(record[0] & 3U);
        if (n == M17_FRAME_SYMBOLS) {
            if (g_vox_rf_count < VOX_RF_MAX) {
                uint8_t eos = 0U;
                g_vox_rf_kind[g_vox_rf_count] = vox_rf_classify(dibits, &eos);
                g_vox_rf_eos[g_vox_rf_count] = eos;
                g_vox_rf_count++;
            }
            n = 0;
        }
    }
}

/* The transmissions the RF holds, past the start's dead air: each a preamble and an LSF, stream frames with the EOS bit
   on the last one only, the EOT marker right after it, then dead air. Fills @p streams and @p dead_after (the stream
   frames and the dead air after the marker of each, up to @p max) and returns how many, or -1 for anything else. */
static int
vox_rf_transmissions(int* streams, int* dead_after, int max) {
    int i = 0;
    while (i < g_vox_rf_count && g_vox_rf_kind[i] == 'D') {
        i++;
    }
    int count = 0;
    while (i < g_vox_rf_count) {
        if (i + 2 > g_vox_rf_count || g_vox_rf_kind[i] != 'P' || g_vox_rf_kind[i + 1] != 'L') {
            return -1;
        }
        i += 2;
        int n = 0;
        while (i < g_vox_rf_count && g_vox_rf_kind[i] == 'S') {
            const int last = i + 1 >= g_vox_rf_count || g_vox_rf_kind[i + 1] != 'S';
            if (g_vox_rf_eos[i] != (last ? 1U : 0U)) {
                return -1;
            }
            n++;
            i++;
        }
        if (n == 0 || i >= g_vox_rf_count || g_vox_rf_kind[i] != 'E') {
            return -1;
        }
        i++;
        int dead = 0;
        while (i < g_vox_rf_count && g_vox_rf_kind[i] == 'D') {
            dead++;
            i++;
        }
        if (count < max) {
            streams[count] = n;
            dead_after[count] = dead;
        }
        count++;
    }
    return count;
}

/* One transmission and its counts (vox_rf_transmissions()): 0, or -1 for anything else. */
static int
vox_rf_ending(int* streams, int* dead_after) {
    return vox_rf_transmissions(streams, dead_after, 1) == 1 ? 0 : -1;
}

static int
fake_vox_read_ex(void* rtl_ctx, float* out, uint8_t* flags, size_t count, int* out_got) {
    (void)rtl_ctx;
    if (out == NULL || out_got == NULL || count != 1U || g_vox_reads >= g_vox_cap) {
        return -1;
    }
    const long index = g_vox_reads++;
    if ((index % VOX_ITER_SAMPLES) == 0 && index / VOX_ITER_SAMPLES < VOX_MAX_ITERS) {
        g_vox_tx[index / VOX_ITER_SAMPLES] = g_vox_state->m17encoder_tx;
    }
    if (index == g_vox_exit_at) {
        exitflag = 1;
    }
    if (index == g_vox_manual_unkey_at) {
        /* What the TX toggle command does (apply_cmd_m17_tx_toggle()). */
        g_vox_state->m17encoder_tx = 0;
        g_vox_state->m17encoder_eot = 1;
    }
    out[0] = (float)next_tone_sample();
    if (flags != NULL) {
        flags[0] = index < g_vox_open_reads ? 0U : (uint8_t)DSD_SQUELCH_FLAG_CLOSED;
    }
    *out_got = 1;
    return 0;
}

static int
fake_vox_read(void* rtl_ctx, float* out, size_t count, int* out_got) {
    return fake_vox_read_ex(rtl_ctx, out, NULL, count, out_got);
}

static double
fake_vox_return_pwr(const void* rtl_ctx) {
    (void)rtl_ctx;
    return (g_vox_low_power_from >= 0 && g_vox_reads > g_vox_low_power_from) ? dB_to_pwr(-90.0) : 1.0;
}

static int
fake_vox_connect(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    return 0;
}

static int
fake_vox_blaster(const dsd_opts* opts, dsd_state* state, size_t nsam, const void* data) {
    (void)opts;
    const uint8_t* bytes = (const uint8_t*)data;
    if (nsam == 54U && bytes != NULL) {
        dsd_call_snapshot call;
        if (g_vox_ip_stream < VOX_MAX_ITERS) {
            g_vox_ip_epoch[g_vox_ip_stream] =
                (dsd_call_state_get(state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE) ? call.epoch : 0U;
        }
        g_vox_ip_stream++;
        /* The EOS bit: after the magic (32 bits), the stream id (16) and the LSF (224). */
        g_vox_ip_eos += (bytes[34] >> 7) & 1U;
    } else if (nsam == 10U && bytes != NULL && bytes[0] == 'E') {
        g_vox_ip_eotx++;
    }
    return 0;
}

static void
vox_run(dsd_opts* opts, dsd_state* state, int mode, int vox) {
    static int token;
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtl_volume_multiplier = 2;
    opts->rtl_squelch_mode = mode;
    opts->rtl_squelch_margin_db = 10;
    opts->rtl_squelch_level = dB_to_pwr(-60.0);
    opts->monitor_input_audio = 1;
    opts->audio_out = 1;
    opts->audio_out_type = g_vox_local_audio ? 0 : 8;
    static int audio_token;
    opts->audio_raw_out = g_vox_local_audio ? (dsd_audio_stream*)&audio_token : NULL;
    opts->m17_use_ip = 1;
    char capture_path[DSD_TEST_PATH_MAX] = {0};
    const int capture_fd = dsd_test_mkstemp(capture_path, sizeof capture_path, "dsdneo_m17_vox_rf");
    if (capture_fd >= 0) {
        (void)dsd_close(capture_fd);
        opts->symbol_out_f = dsd_fopen_private(capture_path, "w+b");
    }
    state->m17_vox = vox;
    state->m17_rate = 8000;
    state->m17_can_en = -1;
    state->rtl_ctx = (struct RtlSdrContext*)&token;
    g_vox_state = state;
    g_vox_reads = 0;
    DSD_MEMSET(g_vox_tx, 0xFF, sizeof g_vox_tx);
    g_vox_ip_stream = 0;
    g_vox_ip_eos = 0;
    g_vox_ip_eotx = 0;
    DSD_MEMSET(g_vox_ip_epoch, 0, sizeof g_vox_ip_epoch);
#ifdef USE_CODEC2
    g_codec2_encode_count = 0;
    DSD_MEMSET(g_codec2_encode_peaks, 0, sizeof g_codec2_encode_peaks);
#endif
    start_tone(1000.0, 0.125, 8000);
    dsd_rtl_stream_io_hooks_set((dsd_rtl_stream_io_hooks){
        .read = fake_vox_read,
        .return_pwr = fake_vox_return_pwr,
        .read_ex = fake_vox_read_ex,
        .squelch_status = fake_rtl_gate_status,
    });
    dsd_m17_udp_hooks_set((dsd_m17_udp_hooks){.connect = fake_vox_connect, .blaster = fake_vox_blaster});
    exitflag = 0;
#ifdef DSD_NEO_TEST_AUDIO_WRAP
    g_audio_event_count = 0;
    g_audio_events[0] = '\0';
    g_audio_wrap_on = g_vox_local_audio;
#endif
    (void)encodeM17STR(opts, state);
#ifdef DSD_NEO_TEST_AUDIO_WRAP
    g_audio_wrap_on = 0;
#endif
    exitflag = 0;
    vox_rf_parse(opts->symbol_out_f);
    if (opts->symbol_out_f) {
        (void)fclose(opts->symbol_out_f);
        opts->symbol_out_f = NULL;
    }
    if (capture_path[0] != '\0') {
        (void)remove(capture_path);
    }
    opts->audio_raw_out = NULL;
    dsd_rtl_stream_io_hooks_set((dsd_rtl_stream_io_hooks){0});
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
    dsd_m17_udp_hooks_set((dsd_m17_udp_hooks){0});
    dsd_state_ext_free_all(state);
}

static void
vox_defaults(long open_iters, long iters) {
    g_vox_open_reads = open_iters * VOX_ITER_SAMPLES;
    g_vox_cap = iters * VOX_ITER_SAMPLES;
    g_vox_low_power_from = -1;
    g_vox_exit_at = -1;
    g_vox_manual_unkey_at = -1;
}

/* The iteration the transmitter was first seen unkeyed at, or -1. */
static int
vox_first_unkeyed(int iters) {
    for (int i = 1; i < iters && i < VOX_MAX_ITERS; i++) {
        if (g_vox_tx[i] == 0) {
            return i;
        }
    }
    return -1;
}

/* Issue #625: under AUTO and NOISE the VOX unkeys on the stream's gate: open for 3 iterations, then closed, it is keyed
   through iteration 18 (11 closed reads end at iteration 13, and the LICH superframe ends at 18) and the stream ends
   there with its EOS frame and the EOT marker, without dead air. Open flags hold it keyed; under LEVEL the power
   decides, as before. */
static int
test_encoder_vox_unkeys_on_the_gate(void) {
    static dsd_opts opts;
    static dsd_state state;
    int err = 0;
    const int modes[2] = {DSD_SQUELCH_MODE_AUTO, DSD_SQUELCH_MODE_NOISE};
    for (int m = 0; m < 2; m++) {
        vox_defaults(3, 24);
        vox_run(&opts, &state, modes[m], 1);
        err |= expect_int("vox gate: the input was read to its end", (int)(g_vox_reads == g_vox_cap), 1);
        err |= expect_int("vox gate: keyed at the start", g_vox_tx[1], 1);
        err |= expect_int("vox gate: unkeyed after 11 closed reads at the LICH boundary", vox_first_unkeyed(24), 19);
        err |= expect_int("vox gate: one stream of 19 frames, the last with EOS", g_vox_ip_stream, 19);
        err |= expect_int("vox gate: one EOS frame", g_vox_ip_eos, 1);
        err |= expect_int("vox gate: one EOTX", g_vox_ip_eotx, 1);
        /* RF: the preamble and LSF, 18 stream frames, the one with the EOS bit and the EOT marker, without dead air. */
        int streams = 0;
        int dead = -1;
        err |= expect_int("vox gate: RF ends with an EOS frame and the EOT marker", vox_rf_ending(&streams, &dead), 0);
        err |= expect_int("vox gate: RF stream frames", streams, 19);
        err |= expect_int("vox gate: no dead air after the marker", dead, 0);
#ifdef USE_CODEC2
        /* Codec2 hears the tone while the gate is open, silence once it closed (the 10 ms ramp, then zeros). */
        int quiet = 1;
        for (int f = 2 * 4; f < g_codec2_encode_count && f < 2 * 18; f++) {
            quiet &= g_codec2_encode_peaks[f] <= 1;
        }
        err |= expect_int("vox gate: codec2 hears the open tone", g_codec2_encode_peaks[1] > 1000, 1);
        err |= expect_int("vox gate: codec2 hears silence once closed", quiet, 1);
#endif
    }
    vox_defaults(24, 24);
    vox_run(&opts, &state, DSD_SQUELCH_MODE_AUTO, 1);
    err |= expect_int("vox gate: open flags stay keyed", vox_first_unkeyed(24), -1);
    /* LEVEL ignores the flags: high power keyed throughout; power dropping after iteration 3 unkeys at 19, as before,
       now with the EOS frame and EOT marker. */
    vox_defaults(0, 24);
    vox_run(&opts, &state, DSD_SQUELCH_MODE_LEVEL, 1);
    err |= expect_int("vox level: power holds it keyed", vox_first_unkeyed(24), -1);
    vox_defaults(0, 24);
    g_vox_low_power_from = 3L * VOX_ITER_SAMPLES;
    vox_run(&opts, &state, DSD_SQUELCH_MODE_LEVEL, 1);
    err |= expect_int("vox level: low power unkeys", vox_first_unkeyed(24), 19);
    err |= expect_int("vox level: one EOS frame", g_vox_ip_eos, 1);
    int streams = 0;
    int dead = -1;
    err |= expect_int("vox level: RF ends with an EOS frame and the EOT marker", vox_rf_ending(&streams, &dead), 0);
    err |= expect_int("vox level: no dead air after the marker", dead, 0);
    /* A manual unkey while VOX still hears a carrier ends that stream (EOS frame, EOT marker, dead air) and VOX opens a
       new one with its own LSF, and its own monitored call, at once; the input ending then ends that one too. */
    vox_defaults(64, 12);
    g_vox_manual_unkey_at = 5L * VOX_ITER_SAMPLES;
    vox_run(&opts, &state, DSD_SQUELCH_MODE_AUTO, 1);
    {
        int tx_streams[4] = {0};
        int tx_dead[4] = {0};
        const int count = vox_rf_transmissions(tx_streams, tx_dead, 4);
        err |= expect_int("manual unkey under VOX: two well-formed transmissions", count, 2);
        err |= expect_int("manual unkey under VOX: the first ends at the unkey", tx_streams[0], 6);
        err |= expect_int("manual unkey under VOX: dead air after the first", tx_dead[0], 25);
        err |= expect_int("manual unkey under VOX: the second ends at the input's end", tx_streams[1], 7);
        err |= expect_int("manual unkey under VOX: dead air after the second", tx_dead[1], 25);
        err |= expect_int("manual unkey under VOX: two IP EOS frames", g_vox_ip_eos, 2);
        err |= expect_int("manual unkey under VOX: two IP EOTX", g_vox_ip_eotx, 2);
        /* Each transmission is one monitored call, and the two are distinct. */
        const int first = tx_streams[0];
        const int second = tx_streams[1];
        int calls_ok = count == 2 && first > 0 && second > 0 && first <= VOX_MAX_ITERS
                       && second <= VOX_MAX_ITERS - first && first + second == g_vox_ip_stream;
        if (calls_ok) {
            const uint64_t call_a = g_vox_ip_epoch[0];
            const uint64_t call_b = g_vox_ip_epoch[first];
            calls_ok = call_a != 0U && call_b != 0U && call_a != call_b;
            for (int i = 0; calls_ok && i < first + second; i++) {
                calls_ok = g_vox_ip_epoch[i] == (i < first ? call_a : call_b);
            }
        }
        err |= expect_int("manual unkey under VOX: one monitored call per transmission", calls_ok, 1);
    }
    return err;
}

/* Issue #625: a transmission still open when the encoder stops ends exactly once: an exit seen while keyed sends that
   frame with its EOS bit and the EOT marker after it; an input that ends sends a silent EOS frame and the marker; a
   manual unkey whose next read fails still flushes, with its dead air. Each with IP's EOTX, never twice. */
static int
test_encoder_ends_an_open_stream_on_shutdown(void) {
    static dsd_opts opts;
    static dsd_state state;
    int err = 0;
    /* Exit seen while keyed, during iteration 5. */
    vox_defaults(64, 64);
    g_vox_exit_at = (5L * VOX_ITER_SAMPLES) + 10;
    vox_run(&opts, &state, DSD_SQUELCH_MODE_LEVEL, 0);
    err |= expect_int("exit: one EOS frame", g_vox_ip_eos, 1);
    err |= expect_int("exit: one EOTX", g_vox_ip_eotx, 1);
    int streams = 0;
    int dead = -1;
    err |= expect_int("exit: RF ends with an EOS frame and the EOT marker", vox_rf_ending(&streams, &dead), 0);
    err |= expect_int("exit: dead air after the marker", dead, 25);
    /* The input ends while keyed. */
    vox_defaults(64, 6);
    vox_run(&opts, &state, DSD_SQUELCH_MODE_LEVEL, 0);
    err |= expect_int("eof: one EOS frame", g_vox_ip_eos, 1);
    err |= expect_int("eof: one EOTX", g_vox_ip_eotx, 1);
    err |= expect_int("eof: frames", g_vox_ip_stream, 6 + 1);
    err |= expect_int("eof: RF ends with an EOS frame and the EOT marker", vox_rf_ending(&streams, &dead), 0);
    err |= expect_int("eof: RF stream frames", streams, 6 + 1);
    err |= expect_int("eof: dead air after the marker", dead, 25);
    /* A manual unkey, then the input ends before its frame. */
    vox_defaults(64, 64);
    g_vox_manual_unkey_at = 4L * VOX_ITER_SAMPLES;
    g_vox_cap = (4L * VOX_ITER_SAMPLES) + 1;
    vox_run(&opts, &state, DSD_SQUELCH_MODE_LEVEL, 0);
    err |= expect_int("manual then eof: one EOS frame", g_vox_ip_eos, 1);
    err |= expect_int("manual then eof: one EOTX", g_vox_ip_eotx, 1);
    err |=
        expect_int("manual then eof: RF ends with an EOS frame and the EOT marker", vox_rf_ending(&streams, &dead), 0);
    err |= expect_int("manual then eof: dead air after the marker", dead, 25);
#ifdef DSD_NEO_TEST_AUDIO_WRAP
    /* On a local output, which may play asynchronously and drop its oldest samples when full, the EOS frame and the
       marker drain before the dead air is queued, and the dead air drains before the encoder returns. */
    g_vox_local_audio = 1;
    vox_defaults(64, 64);
    g_vox_manual_unkey_at = 4L * VOX_ITER_SAMPLES;
    g_vox_cap = (4L * VOX_ITER_SAMPLES) + 1;
    vox_run(&opts, &state, DSD_SQUELCH_MODE_LEVEL, 0);
    g_vox_local_audio = 0;
    {
        static const char want_tail[] = "WWDZZZZZZZZZZZZZZZZZZZZZZZZZDD";
        const size_t len = strlen(g_audio_events);
        const int ok = len >= sizeof want_tail - 1U
                       && strcmp(g_audio_events + (len - (sizeof want_tail - 1U)), want_tail) == 0
                       && strchr(g_audio_events, 'D') == g_audio_events + (len - (sizeof want_tail - 1U)) + 2;
        if (!ok) {
            DSD_FPRINTF(stderr, "local output events: %s\n", g_audio_events);
        }
        err |= expect_int("manual then eof: the end drains around the dead air on a local output", ok, 1);
    }
    /* A VOX unkey sends no dead air and drains nothing then; the input stopping right after it drains what was queued
       before the output closes. */
    g_vox_local_audio = 1;
    vox_defaults(3, 19);
    vox_run(&opts, &state, DSD_SQUELCH_MODE_AUTO, 1);
    g_vox_local_audio = 0;
    {
        const size_t len = strlen(g_audio_events);
        const char* first_drain = strchr(g_audio_events, 'D');
        const int ok =
            len >= 3U && strcmp(g_audio_events + (len - 3U), "WWD") == 0 && first_drain == g_audio_events + (len - 1U);
        if (!ok) {
            DSD_FPRINTF(stderr, "vox unkey then eof local output events: %s\n", g_audio_events);
        }
        err |= expect_int("vox unkey then eof: one drain, after the EOS frame and the marker", ok, 1);
    }
#endif
    /* Nothing open: an encoder that never keyed sends no end. */
    vox_defaults(0, 6);
    vox_run(&opts, &state, DSD_SQUELCH_MODE_AUTO, 1);
    err |= expect_int("never keyed: no stream", g_vox_ip_stream + g_vox_ip_eotx, 0);
    return err;
}
#endif

int
main(void) {
    int err = 0;
    err |= test_encoder_vox_helpers();
#ifdef DSD_NEO_TEST_AUDIO_WRAP
    err |= test_packet_encoder_end_drains_on_a_local_output();
#endif
#ifdef USE_RADIO
    err |= test_encoder_rtl_reader_gates_closed_samples();
    err |= test_encoder_vox_unkeys_on_the_gate();
    err |= test_encoder_ends_an_open_stream_on_shutdown();
#endif
    err |= test_encoder_silent_udp_gives_way_to_a_queued_command();
    err |= test_embedded_lich_chunks_store_and_finalize_lsf_state();
    err |= test_embedded_lich_rejects_invalid_counter_and_gates_bad_lsf_crc();
    err |= test_rf_lsf_crc_policy_preserves_relaxed_decode();
    err |= test_lsf_crypto_availability_matches_payload_validation();
    err |= test_lsf_application_resets_and_stores_state();
    err |= test_lsf_rejects_reserved_type_without_replacing_state();
    err |= test_lsf_rejects_invalid_addresses_without_replacing_state();
    err |= test_lsf_null_meta_decodes_text_and_honors_can_filter();
    err |= test_stream_dispatch_can_filter_and_aes_gates();
    err |= test_stream_dispatch_rejects_invalid_arguments_and_unknown_encryption();
    err |= test_stream_dispatch_scrambler_decrypts_with_seed_and_subtype();
    err |= test_stream_signature_frames_are_consumed_and_verify_without_key();
    err |= test_stream_signature_out_of_order_marks_sequence_error();
    err |= test_clear_signed_payload_updates_digest_and_dispatches();
    err |= test_stream_voice_replaces_foreign_active_call();
    err |= test_lsf_crc_confirms_the_transmission();
    err |= test_unconfirmed_stream_publishes_no_call();
    err |= test_stream_voice_chain_covers_the_whole_frame();
    err |= test_stream_voice_chain_onepoles_run_at_8k();
    err |= test_encoder_pcm_input_is_anti_aliased();
#ifdef USE_RADIO
    err |= test_encoder_rtl_input_reaches_pcm_scale();
#endif
#ifdef USE_CODEC2
    err |= test_stream_voice_3200_dispatch_routes_pair_audio_to_udp();
    err |= test_stream_voice_1600_dispatch_routes_single_audio_to_udp();
    err |= test_stream_voice_audio_gate_suppresses_udp_when_slot_disabled();
    err |= test_stream_voice_audio_honors_mute_on_every_output();
#endif
    err |= test_bert_payload_locks_from_default_state_and_continues();
    err |= test_bert_payload_resyncs_after_error_threshold();
    err |= test_bert_hard_payload_decode_primitives();
    err |= test_frame_info_packet_and_ip_helpers();
    err |= test_ip_stream_frames_apply_crc_gated_lsf_state();
    err |= test_ip_stream_bad_crc_does_not_reopen_ended_voice_epoch();
    err |= test_ip_mpkt_frames_apply_crc_gated_packet_state();
    err |= test_packet_eot_finalization_crc_gates_decode_and_clears_state();
    err |= test_packet_eot_preserves_non_packet_calls();
    err |= test_encoder_control_lsf_and_dibit_helpers();
    err |= test_packet_protocol_identifier_utf8_boundaries();
    err |= test_m17_hook_argument_guards();
    err |= test_encoders_propagate_requested_udp_setup_failure();
    err |= test_local_stream_lsf_publishes_canonical_identity();
    err |= test_monitored_stream_call_follows_tx_lifecycle();
    err |= test_packet_encoder_monitors_lsf_with_canonical_viterbi();
    err |= test_stream_encoder_treats_stdin_eof_as_clean_shutdown();
    err |= test_ip_decoder_propagates_udp_backend_failure();

    if (err == 0) {
        printf("M17_STATE_DISPATCH: OK\n");
    }
    return err;
}

// NOLINTEND(bugprone-implicit-widening-of-multiplication-result)
