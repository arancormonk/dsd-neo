// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// or invalid-value negative vectors to exercise guarded behavior.
// NOLINTBEGIN(bugprone-implicit-widening-of-multiplication-result)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "fixtures/m17_reference_vectors.h"

#include <assert.h>
#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/audio_activity.h>
#include <dsd-neo/core/audio_filters.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/voice_chain.h>
#include <dsd-neo/dsp/analog_rx.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/input_backlog.h>
#include <dsd-neo/platform/pcm_stream.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/protocol/m17/m17.h>
#include <dsd-neo/runtime/control_pump.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/m17_udp_hooks.h>
#include <dsd-neo/runtime/net_audio_input_hooks.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#ifdef USE_RADIO
#include <dsd-neo/runtime/rtl_stream_io_hooks.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#endif
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "core/audio/voice_chain_internal.h"
#include "dsd-neo/core/dibit.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/dsp/analog_voice.h"
#include "dsd-neo/dsp/speech_agc.h"
#include "dsd-neo/platform/audio.h"
#include "dsd-neo/platform/sockets.h"
#include "dsd-neo/protocol/m17/m17_parse.h"
#include "dsd-neo/protocol/m17/m17_tables.h"
#include "m17_algorithms.h"
#include "m17_confirm.h"
#include "m17_internal.h"
#include "m17_state_ext.h"
#include "test_support.h"
#if DSD_PLATFORM_WIN_NATIVE
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

struct CODEC2;

static dsd_opts g_opts;
static dsd_state g_state;
/* Codec2 decodes of received or monitored voice: the playback attempts. Declared in every build so the lifecycle cases
   run without Codec2 too; with no decoder nothing decodes, and it stays 0 (issue #890). */
static int g_codec2_decode_calls;
#ifdef USE_CODEC2
static unsigned char g_codec2_last_bits[8];
static int g_udp_audio_calls;
static size_t g_udp_audio_last_nsam;
static const dsd_opts* g_udp_audio_last_opts;
static dsd_state* g_udp_audio_last_state;
static short g_udp_audio_last_first_sample;
static uint8_t g_udp_audio_last_data[320U * 2U * sizeof(float)];
/* Samples the fake decoder writes per frame: codec2's real output, 160 at 3200 bit/s and 320 at 1600 (issue #658). */
static size_t g_codec2_decode_samples = 160U;
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
        for (size_t i = 0U; i < g_codec2_decode_samples; i++) {
            speech[i] = (short)(1000 + g_codec2_decode_calls + (int)i);
        }
    }
}

/* The fake codec2 1600 a test may hand the encoder (dsd_state::codec2_1600): its frames take 320 samples, as the real
   one's do; any other codec2 state takes 160. */
static int g_codec2_1600_token;
#define TEST_CODEC2_1600 ((struct CODEC2*)&g_codec2_1600_token)

int
codec2_samples_per_frame(struct CODEC2* codec2_state) {
    return codec2_state == TEST_CODEC2_1600 ? 320 : 160;
}

/* The peak and the RMS of each frame the stream encoder hands codec2, in order (issue #625: closed samples reach it as
   silence). */
enum { TEST_CODEC2_ENCODE_LOG = 512 };

static int g_codec2_encode_count = 0;
static int g_codec2_encode_peaks[TEST_CODEC2_ENCODE_LOG];
static double g_codec2_encode_rms[TEST_CODEC2_ENCODE_LOG];

void
codec2_encode(struct CODEC2* codec2_state, unsigned char* bits, short speech[]) {
    (void)bits;
    const size_t nsam = (size_t)codec2_samples_per_frame(codec2_state);
    int peak = 0;
    double acc = 0.0;
    for (size_t i = 0U; speech != NULL && i < nsam; i++) {
        const int v = speech[i] < 0 ? -(int)speech[i] : (int)speech[i];
        peak = v > peak ? v : peak;
        acc += (double)v * (double)v;
    }
    if (g_codec2_encode_count < TEST_CODEC2_ENCODE_LOG) {
        g_codec2_encode_peaks[g_codec2_encode_count] = peak;
        g_codec2_encode_rms[g_codec2_encode_count] = sqrt(acc / (double)nsam);
    }
    g_codec2_encode_count++;
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
    if (data != NULL) {
        DSD_MEMCPY(g_udp_audio_last_data, data,
                   nsam < sizeof(g_udp_audio_last_data) ? nsam : sizeof(g_udp_audio_last_data));
    }
}
#endif

static void
reset_audio_fakes(void) {
    g_codec2_decode_calls = 0;
#ifdef USE_CODEC2
    dsd_udp_audio_hooks hooks = {0};
    dsd_udp_audio_hooks_set(hooks);
    DSD_MEMSET(g_codec2_last_bits, 0, sizeof(g_codec2_last_bits));
    g_udp_audio_calls = 0;
    g_udp_audio_last_nsam = 0U;
    g_udp_audio_last_opts = NULL;
    g_udp_audio_last_state = NULL;
    g_udp_audio_last_first_sample = 0;
    DSD_MEMSET(g_udp_audio_last_data, 0, sizeof(g_udp_audio_last_data));
    g_codec2_decode_samples = 160U;
#endif
}

#ifdef USE_CODEC2
/* Decoded voice plays through the digital voice output (issue #658), which stages each frame in the vocoder buffers
   initState() sets up: an output history, the auto gain's peak history, a mono stream at the fixed gain -g 1 (unity),
   with no high-pass. A zeroed state has none of them and plays nothing. */
static short g_voice_history[100U + (160U * 16U)];

static void
prepare_voice_output(dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(g_voice_history, 0, sizeof(g_voice_history));
    state->audio_out_buf = g_voice_history;
    state->audio_out_buf_p = g_voice_history + 100;
    opts->audio_gain = 1.0f;
    opts->pulse_digi_out_channels = 1;
    opts->pulse_digi_rate_out = 8000;
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
    const size_t frame_len = 34U + app_len + 2U;
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
    err |= expect_int("relaxed RF LSF notes no received identity", (int)state->rx_id.valid, 0);

    /* A checked LSF's CAN is the carrier's; a later one -F decoded despite its failed CRC never replaces it. */
    uint8_t checked_bits[TEST_M17_LSF_BITS];
    uint8_t checked_packed[M17_LSF_BYTES];
    build_lsf_bits(checked_bits, dst, src, m17_compose_frame_info(1U, 2U, 0U, 0U, 3U, 0U, 0U), NULL);
    const uint16_t checked_crc = m17_attach_lsf_crc(checked_bits, checked_packed);
    dsd_state_ext_free_all(state);
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMCPY(state->m17_lsf, checked_bits, sizeof(checked_bits));
    err |= expect_int("checked RF LSF passes", m17_finalize_lsf_crc(opts, state, checked_packed, checked_crc), 0);
    err |= expect_int("checked RF LSF identity parts", (int)state->rx_id.valid, (int)DSD_RXID_ACCESS);
    err |= expect_u8("checked RF LSF identity CAN", (uint8_t)state->rx_id.access, 3U);
    err |= expect_u8("checked RF LSF identity kind", state->rx_id.access_kind, DSD_ACCESS_CODE_CAN);
    DSD_MEMCPY(state->m17_lsf, lsf_bits, sizeof(lsf_bits));
    (void)m17_finalize_lsf_crc(opts, state, lsf_packed, crc ^ 1U);
    /* Nor the live fields the checked LSF bound to the transmission: -F shows it, nothing more (issue #890). */
    err |= expect_u8("relaxed RF LSF keeps the checked live CAN", state->m17_can, 3U);
    err |= expect_u8("relaxed RF LSF keeps the checked encryption", state->m17_enc, 0U);
    err |= expect_u8("relaxed RF LSF keeps the checked CAN", (uint8_t)state->rx_id.access, 3U);
    dsd_state_ext_free_all(state);
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
    // The CRC-checked LSF's encryption type is received; its subtype is no key id.
    err |= expect_u8("LSF encryption type received", call.crypto_received, (uint8_t)DSD_CALL_CRYPTO_RX_ALG);
    err |= expect_u8("LSF received encryption type", call.received_algid, 2U);

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
test_lsf_application_stores_state_and_keeps_signature(void) {
    dsd_state* state = &g_state;
    DSD_MEMSET(state, 0, sizeof(*state));
    state->m17_can = 3U;
    state->m17_enc = 1U;
    state->m17_payload_decrypted = 1U;
    state->m17_signature_received_mask = 0x03U;
    state->m17_signature_complete = 0U;
    state->m17_signature_bad_sequence = 1U;
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
    err |= expect_bytes("LSF meta nonce", state->m17_meta, M17_REF_AES_NONCE, sizeof(M17_REF_AES_NONCE));
    /* An LSF applied mid-stream (a LICH rebuild every six frames, an IP stream's every frame) is no transmission
       boundary: the transmitter signs the digest of every frame since the stream began (M17 spec 2.0.11, Digital
       Signatures), so the digest and the signature collected so far stay (issue #890). */
    err |= expect_u8("LSF keeps the signature mask", state->m17_signature_received_mask, 0x03U);
    err |= expect_u8("LSF keeps the signature sequence", state->m17_signature_bad_sequence, 1U);
    for (size_t i = 0U; i < sizeof(state->m17_signature_digest); i++) {
        err |= expect_u8("LSF keeps the digest", state->m17_signature_digest[i], 0xA5U);
    }
    for (size_t i = 0U; i < sizeof(state->m17_signature); i++) {
        err |= expect_u8("LSF keeps the signature buffer", state->m17_signature[i], 0x5AU);
    }

    /* A transmission boundary forgets them. */
    m17_confirm_reset(state);
    err |= expect_u8("boundary signature advertised reset", state->m17_signature_advertised, 0U);
    err |= expect_u8("boundary signature mask reset", state->m17_signature_received_mask, 0U);
    err |= expect_u8("boundary signature sequence reset", state->m17_signature_bad_sequence, 0U);
    err |= expect_u8("boundary signature status reset", state->m17_signature_verification_status, 0U);
    for (size_t i = 0U; i < sizeof(state->m17_signature_digest); i++) {
        err |= expect_u8("boundary digest reset", state->m17_signature_digest[i], 0U);
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

/* A null-encryption LSF's META: decoded only once its CRC passed and its call was published, never by applying the
 * LSF alone or from one -F decoded despite a failed CRC (issue #800 2.2). */
static int
finalize_meta_lsf(dsd_opts* opts, dsd_state* state, uint8_t subtype, const uint8_t meta[M17_AES_NONCE_BYTES],
                  int corrupt) {
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t lsf_packed[M17_LSF_BYTES];
    build_lsf_bits(lsf_bits, 0x0000009FDD51ULL, 0x0000009FDD51ULL,
                   m17_compose_frame_info(1U, 2U, 0U, subtype, 9U, 0U, 0U), meta);
    const uint16_t crc = m17_attach_lsf_crc(lsf_bits, lsf_packed);
    DSD_MEMCPY(state->m17_lsf, lsf_bits, sizeof(lsf_bits));
    return m17_finalize_lsf_crc(opts, state, lsf_packed, corrupt ? (uint16_t)(crc ^ 1U) : crc);
}

/* The state's M17 extension, created when the decoder made none yet (a test seeds what the decoder assembles). */
static m17_state_ext*
test_m17_ext(dsd_state* state) {
    m17_state_ext* ext = m17_state_ext_find(state);
    if (ext == NULL) {
        ext = (m17_state_ext*)calloc(1U, sizeof(*ext));
        if (ext == NULL || dsd_state_ext_set(state, DSD_STATE_EXT_PROTO_M17_GNSS, ext, free) != 0) {
            free(ext);
            DSD_FPRINTF(stderr, "M17 extension alloc failed\n");
            exit(1);
        }
    }
    return ext;
}

/* The text META blocks the decoder holds: none when it made no extension. */
static uint8_t
text_meta_received(const dsd_state* state) {
    const m17_state_ext* ext = m17_state_ext_find(state);
    return ext != NULL ? ext->text_meta.received_bitmap : 0U;
}

/* The same LSF as finalize_meta_lsf()'s, rebuilt whole from its six LICH chunks: a checked LSF within a transmission,
   which is no boundary. */
static void
rebuild_meta_lsf(dsd_opts* opts, dsd_state* state, uint8_t subtype, const uint8_t meta[M17_AES_NONCE_BYTES]) {
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t lsf_packed[M17_LSF_BYTES];
    uint8_t encoded[M17_LICH_BITS];
    build_lsf_bits(lsf_bits, 0x0000009FDD51ULL, 0x0000009FDD51ULL,
                   m17_compose_frame_info(1U, 2U, 0U, subtype, 9U, 0U, 0U), meta);
    (void)m17_attach_lsf_crc(lsf_bits, lsf_packed);
    for (uint8_t chunk = 0U; chunk < M17_LICH_CHUNKS; chunk++) {
        build_encoded_lich_chunk(lsf_bits, chunk, encoded);
        (void)m17_process_lich(state, opts, encoded);
    }
}

/* Seed the text being assembled: four-block text, block 2 in, every byte 0x5A. */
static void
seed_text_meta(dsd_state* state) {
    m17_state_ext* ext = test_m17_ext(state);
    ext->text_meta.expected_bitmap = 0x0FU;
    ext->text_meta.received_bitmap = 0x02U;
    ext->text_meta.control_or = 0xF2U;
    DSD_MEMSET(ext->text_meta.text, 0x5AU, sizeof(ext->text_meta.text));
}

/* The seeded text is still there, untouched. */
static int
expect_seeded_text_meta(const char* label, const dsd_state* state) {
    const m17_state_ext* ext = m17_state_ext_find(state);
    int err = 0;
    err |= expect_int(label, ext != NULL, 1);
    if (ext == NULL) {
        return err;
    }
    err |= expect_u8(label, ext->text_meta.expected_bitmap, 0x0FU);
    err |= expect_u8(label, ext->text_meta.received_bitmap, 0x02U);
    err |= expect_u8(label, ext->text_meta.control_or, 0xF2U);
    for (size_t i = 0U; i < sizeof(ext->text_meta.text); i++) {
        err |= expect_u8(label, ext->text_meta.text[i], 0x5AU);
    }
    return err;
}

static int
test_lsf_null_meta_decodes_text_and_honors_can_filter(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t expected_text[M17_TEXT_BLOCK_BYTES];
    DSD_MEMSET(expected_text, 0, sizeof(expected_text));
    DSD_MEMCPY(expected_text, "LSF-META-TEXT", M17_TEXT_BLOCK_BYTES);
    uint8_t meta[M17_AES_NONCE_BYTES];
    DSD_MEMSET(meta, 0, sizeof(meta));
    meta[0] = 0x11U;
    DSD_MEMCPY(meta + 1, expected_text, M17_TEXT_BLOCK_BYTES);

    struct m17_lsf_result res = valid_lsf_result();
    res.et = 0U;
    res.es = 0U;
    res.has_meta = 1U;
    res.meta_is_iv = 0U;
    res.cn = 9U;
    DSD_MEMCPY(res.meta, meta, sizeof(res.meta));

    dsd_state_ext_free_all(state);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    state->m17_can_en = -1;

    int err = 0;
    /* Applying the LSF sets the decoder up and stores the META; it decodes none of it. */
    err |= expect_int("null META LSF accepted", m17_apply_lsf_result(state, &res), 1);
    err |= expect_bytes("null META payload stored", state->m17_meta, res.meta, sizeof(res.meta));
    err |= expect_u8("applied LSF decodes no META text", text_meta_received(state), 0U);

    err |= expect_int("checked null META LSF passes", finalize_meta_lsf(opts, state, 0U, meta, 0), 0);
    const m17_state_ext* ext = m17_state_ext_find(state);
    err |= expect_int("null META text assembled in the M17 extension", ext != NULL, 1);
    if (ext != NULL) {
        err |= expect_u8("null META text expected bitmap", ext->text_meta.expected_bitmap, 0x01U);
        err |= expect_u8("null META text received bitmap", ext->text_meta.received_bitmap, 0x01U);
        err |= expect_u8("null META text control", ext->text_meta.control_or, 0x11U);
        err |= expect_bytes("null META text bytes", ext->text_meta.text, expected_text, sizeof(expected_text));
    }

    /* -F decodes an LSF whose CRC failed, but not its META. */
    dsd_state_ext_free_all(state);
    DSD_MEMSET(state, 0, sizeof(*state));
    state->m17_can_en = -1;
    err |= expect_int("relaxed bad null META LSF fails its CRC", finalize_meta_lsf(opts, state, 0U, meta, 1), 1);
    err |= expect_u8("relaxed bad LSF still decodes its CAN", state->m17_can, 9U);
    err |= expect_u8("relaxed bad LSF decodes no META text", text_meta_received(state), 0U);

    /* A CAN-filtered or reserved META within the transmission leaves the text being assembled alone. */
    dsd_state_ext_free_all(state);
    DSD_MEMSET(state, 0, sizeof(*state));
    state->m17_can_en = 4;
    seed_text_meta(state);
    rebuild_meta_lsf(opts, state, 0U, meta);
    err |= expect_u8("CAN-filtered LSF is checked", state->m17_lsf_checked, 1U);
    err |= expect_seeded_text_meta("CAN-filtered null META preserves the text", state);

    dsd_state_ext_free_all(state);
    DSD_MEMSET(state, 0, sizeof(*state));
    state->m17_can_en = -1;
    seed_text_meta(state);
    rebuild_meta_lsf(opts, state, 3U, meta);
    err |= expect_u8("reserved-subtype LSF is checked", state->m17_lsf_checked, 1U);
    err |= expect_seeded_text_meta("reserved null META preserves the text", state);

    /* An LSF frame starts a transmission: the text the last one was assembling goes. */
    err |= expect_int("an LSF frame passes", finalize_meta_lsf(opts, state, 3U, meta, 0), 0);
    err |= expect_u8("an LSF frame empties the text being assembled", text_meta_received(state), 0U);
    dsd_state_ext_free_all(state);
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
    /* A signature is judged only over a whole voice run from frame 0 (issue #890): the stream's one voice frame. */
    DSD_MEMSET(payload_bits, 0, sizeof(payload_bits));
    (void)m17_dispatch_stream_payload(opts, state, payload_bits, 0U, processed_bits);
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
                      m17_dispatch_stream_payload(opts, state, payload_bits, 0U, processed_bits),
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
    m17_confirm_note_evidence(NULL, state, M17_EVIDENCE_STRONG);
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
    /* Issue #890: the scan's voice credit is the speech Codec2 decoded, never the media mark before the payload is
       decoded: a build without Codec2 decodes nothing, so its stream is never checked media. With no scan running
       nobody reads it, and the voice path keeps none. */
    err |= expect_int("late-entry M17 voice, no scan: no checked media", call.checked_media_updated_m > 0.0, 0);
    opts->trunk_scan_enabled = 1;
    /* Under a scan, speech Codec2 decoded is checked speech only once a checked LSF bound the transmission's type,
       encryption and CAN (m17_lsf_checked): before that (a stream joined on its LICH, or one -F set up from an LSF whose
       CRC failed, which may name ciphertext clear voice) it plays but earns no checked media. */
    err |= expect_int("late-entry M17 voice dispatches under a scan",
                      m17_dispatch_stream_payload(opts, state, payload_bits, M17_REF_STREAM_FN, processed_bits),
                      M17_STREAM_CLEAR_DISPATCHED);
    (void)get_m17_call(state, &call);
    err |= expect_int("unchecked-LSF M17 speech earns no checked media", call.checked_media_updated_m > 0.0, 0);
    /* A checked LSF a clean LICH chunk has since disputed (another transmission after a missed EOT and LSF frame) vouches
       for nothing: its speech plays but earns no checked media either. */
    state->m17_lsf_checked = 1U;
    m17_state_ext* link = test_m17_ext(state);
    if (link == NULL) {
        return 1;
    }
    link->lsf_reconfirmed = 0x7U; /* every identity chunk agreed since the stream was continuous */
    link->lsf_disputed = 1U;
    err |= expect_int("disputed-LSF M17 voice dispatches under a scan",
                      m17_dispatch_stream_payload(opts, state, payload_bits, M17_REF_STREAM_FN, processed_bits),
                      M17_STREAM_CLEAR_DISPATCHED);
    (void)get_m17_call(state, &call);
    err |= expect_int("disputed-LSF M17 speech earns no checked media", call.checked_media_updated_m > 0.0, 0);
    link->lsf_disputed = 0U;
    err |= expect_int(
        "checked-LSF M17 voice dispatches under a scan",
        m17_dispatch_stream_payload(opts, state, payload_bits, (uint16_t)(M17_REF_STREAM_FN + 1U), processed_bits),
        M17_STREAM_CLEAR_DISPATCHED);
    (void)get_m17_call(state, &call);
#ifdef USE_CODEC2
    err |= expect_int("checked-LSF M17 speech Codec2 decoded is checked media", call.checked_media_updated_m > 0.0, 1);
#else
    err |= expect_int("late-entry M17 voice with no Codec2 is no checked media", call.checked_media_updated_m > 0.0, 0);
#endif
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
    prepare_voice_output(opts, state);

    opts->slot1_on = 1;
    opts->audio_out = 1;
    opts->audio_out_type = 8;
    state->m17_str_dt = 2U;
    state->m17_can_en = -1;
    /* Over the air an LSF CRC clears before the first stream frame; without that the media gate
     * holds the call and the audio back (#399). */
    m17_confirm_note_evidence(NULL, state, M17_EVIDENCE_STRONG);
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
    prepare_voice_output(opts, state);
    g_codec2_decode_samples = 320U;

    opts->slot1_on = 1;
    opts->audio_out = 1;
    opts->audio_out_type = 8;
    state->m17_str_dt = 3U;
    state->m17_can_en = -1;
    /* Over the air an LSF CRC clears before the first stream frame; without that the media gate
     * holds the call and the audio back (#399). */
    m17_confirm_note_evidence(NULL, state, M17_EVIDENCE_STRONG);
    bytes_to_bits(payload_bytes, payload_bits, sizeof(payload_bytes));

    int err = 0;
    err |= expect_int("1600 voice stream dispatches",
                      m17_dispatch_stream_payload(opts, state, payload_bits, M17_REF_STREAM_FN, processed_bits),
                      M17_STREAM_CLEAR_DISPATCHED);
    err |= expect_bytes("1600 voice processed bits", processed_bits, payload_bits, sizeof(payload_bits));
    err |= expect_int("1600 voice Codec2 decodes", g_codec2_decode_calls, 1);
    err |= expect_bytes("1600 voice codec frame", g_codec2_last_bits, payload_bytes, 8U);
    /* Codec2's 320 samples go out as two 160-sample frames, the second starting at the decoder's sample 160. */
    err |= expect_int("1600 voice UDP calls", g_udp_audio_calls, 2);
    err |= expect_int("1600 voice UDP bytes", (int)g_udp_audio_last_nsam, (int)(160U * sizeof(short)));
    err |= expect_int("1600 voice UDP second frame first sample", (int)g_udp_audio_last_first_sample, 1001 + 160);
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
    prepare_voice_output(opts, state);

    opts->slot1_on = 0;
    opts->audio_out = 1;
    opts->audio_out_type = 8;
    state->m17_str_dt = 2U;
    state->m17_can_en = -1;
    /* Over the air an LSF CRC clears before the first stream frame; without that the media gate
     * holds the call and the audio back (#399). */
    m17_confirm_note_evidence(NULL, state, M17_EVIDENCE_STRONG);
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

    /* With no network stream of that station open, DISC and EOTX end nothing (issue #890; the matching case is
       test_ip_end_controls_match_the_station()). */
    DSD_MEMSET(state, 0, sizeof(*state));
    state->carrier = 1;
    state->synctype = DSD_SYNC_M17_STR_POS;
    uint8_t disc[10] = {'D', 'I', 'S', 'C', 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB};
    m17_ip_dispatch_frame(opts, state, disc, sizeof(disc));
    err |= expect_int("DISC of no open stream keeps carrier", state->carrier, 1);
    err |= expect_int("DISC of no open stream keeps synctype", state->synctype, DSD_SYNC_M17_STR_POS);

    uint8_t eotx[10] = {'E', 'O', 'T', 'X', 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB};
    m17_ip_dispatch_frame(opts, state, eotx, sizeof(eotx));
    err |= expect_int("EOTX of no open stream keeps carrier", state->carrier, 1);
    err |= expect_int("EOTX of no open stream keeps synctype", state->synctype, DSD_SYNC_M17_STR_POS);

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
test_ip_frames_reject_truncation_before_reading_or_changing_state(void) {
    static const struct {
        uint8_t magic[4];
        size_t minimum;
    } cases[] = {{{'M', '1', '7', ' '}, 54U}, {{'M', 'P', 'K', 'T'}, 37U}, {{'C', 'O', 'N', 'N'}, 11U},
                 {{'D', 'I', 'S', 'C'}, 10U}, {{'E', 'O', 'T', 'X'}, 10U}, {{'P', 'I', 'N', 'G'}, 10U},
                 {{'P', 'O', 'N', 'G'}, 10U}, {{'A', 'C', 'K', 'N'}, 4U},  {{'N', 'A', 'C', 'K'}, 4U}};

    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts != NULL && state != NULL);
    opts->payload = 1;
    state->carrier = 1;
    state->synctype = DSD_SYNC_M17_STR_POS;
    state->m17_lsf[0] = 1U;
    int err = 0;
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; ++c) {
        for (size_t len = 0; len < cases[c].minimum; ++len) {
            /* Exact-sized allocations let ASan see any read beyond the received datagram, including its magic. */
            uint8_t* frame = calloc(len > 0U ? len : 1U, 1);
            assert(frame != NULL);
            DSD_MEMCPY(frame, cases[c].magic, len < 4U ? len : 4U);
            m17_ip_dispatch_frame(opts, state, frame, (int)len);
            err |= expect_int("truncated IP frame keeps carrier", state->carrier, 1);
            err |= expect_int("truncated IP frame keeps sync", state->synctype, DSD_SYNC_M17_STR_POS);
            err |= expect_u8("truncated IP frame preserves LSF", state->m17_lsf[0], 1U);
            free(frame);
        }
    }
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    return err;
}

static int
test_short_valid_ip_mpkt_reads_only_its_header(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts != NULL && state != NULL);
    opts->payload = 1;
    state->m17_can_en = -1;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    const uint8_t app[] = {0x05U, 0U};
    const size_t length = 34U + sizeof(app) + 2U;
    uint8_t* frame = malloc(length);
    assert(frame != NULL);
    const unsigned long long dst = m17_encode_b40_callsign(0ULL, M17_REF_LSF_DST_CSD);
    const unsigned long long src = m17_encode_b40_callsign(0ULL, M17_REF_LSF_SRC_CSD);
    const uint16_t type_word = m17_compose_frame_info(0U, 0U, 0U, 0U, 5U, 0U, 0U);
    build_lsf_bits(lsf_bits, dst, src, type_word, NULL);
    assert(build_ip_mpkt_frame(frame, length, lsf_bits, 0xCAFEU, app, sizeof(app)) == length);
    m17_ip_dispatch_frame(opts, state, frame, (int)length);
    dsd_call_snapshot call;
    int err = expect_int("short valid MPKT publishes identity", get_m17_call(state, &call), 1);
    err |= expect_u64("short valid MPKT destination", call.ota_target_id, dst);
    err |= expect_u64("short valid MPKT source", call.ota_source_id, src);
    err |= expect_int("short valid MPKT ends call", call.phase, DSD_CALL_PHASE_ENDED);
    free(frame);
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    return err;
}

static int
test_ip_stream_frames_apply_crc_gated_lsf_state(void) {
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
    build_ip_stream_frame(ip_frame, lsf_bits, 0xBEEF, M17_REF_STREAM_FN, 1U, payload_bits);
    ip_frame[52] ^= 0x01U;

    /* An IP frame whose CRC failed is nothing of the stream's (issue #890): no carrier, sync type or AES counter. */
    m17_ip_dispatch_frame(opts, state, ip_frame, sizeof(ip_frame));
    err |= expect_int("bad IP stream CRC sets no carrier", state->carrier, 0);
    err |= expect_int("bad IP stream CRC sets no synctype", state->synctype, 0);
    err |= expect_int("bad data IP stream CRC does not publish call", get_m17_call(state, &call), 0);
    err |= expect_u8("bad IP stream CRC preserves enc", state->m17_enc, 0U);
    err |= expect_u8("bad IP stream CRC stores no counter high", state->m17_meta[14], 0x5AU);
    err |= expect_u8("bad IP stream CRC stores no counter low", state->m17_meta[15], 0x5AU);
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
    uint8_t app[2U + M17_META_BYTES];
    uint8_t mpkt_frame[34U + sizeof(app) + 2U];
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
    /* The sender emits the header, the application bytes and CRC2 only. Fill the whole text block so a dropped
       final application byte cannot be hidden by a NUL or by padding in the frame builder. */
    DSD_MEMCPY(app + 3, "IP-MPKT-LAST!", M17_TEXT_BLOCK_BYTES);
    DSD_MEMCPY(expected_text, "IP-MPKT-LAST!", M17_TEXT_BLOCK_BYTES);
    const size_t mpkt_len = build_ip_mpkt_frame(mpkt_frame, sizeof(mpkt_frame), lsf_bits, 0xCAFEU, app, sizeof(app));

    state->m17_can_en = -1;
    dsd_test_capture_stderr cap;
    char out[8192];
    if (dsd_test_capture_stderr_begin(&cap, "m17_mpkt_text") != 0) {
        return 1;
    }
    m17_ip_dispatch_frame(opts, state, mpkt_frame, (int)mpkt_len);
    (void)dsd_test_capture_stderr_end(&cap);
    (void)dsd_test_capture_stderr_read(&cap, out, sizeof(out));

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
    /* The MPKT's own LSF named its call (CAN 5), and its settings went with it: the decoded link setup is what it
       found (issue #890). */
    err |= expect_int("valid MPKT call carries its CAN", (int)call.service_options, 5);
    err |= expect_u8("valid MPKT gives back the stream type", state->m17_str_dt, 0U);
    err |= expect_u8("valid MPKT gives back the CAN", state->m17_can, 0U);
    err |= expect_u8("valid MPKT gives back the checked binding", state->m17_lsf_checked, 0U);
    /* Its one-block text completes, final application byte included, and goes with the MPKT: one MPKT is one whole
       transmission. */
    err |= expect_int("valid MPKT text completes with its final application byte",
                      strstr(out, "Complete: IP-MPKT-LAST!") != NULL, 1);
    err |= expect_u8("valid MPKT text ends with it", text_meta_received(state), 0U);
    (void)expected_text;

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
    seed_text_meta(state);
    build_ip_mpkt_frame(mpkt_frame, sizeof(mpkt_frame), lsf_bits, 0xCAFEU, app, sizeof(app));
    mpkt_frame[mpkt_len - 1U] ^= 0x01U;

    m17_ip_dispatch_frame(opts, state, mpkt_frame, (int)mpkt_len);
    err |= expect_int("bad MPKT CRC does not publish call", get_m17_call(state, &call), 0);
    err |= expect_seeded_text_meta("bad MPKT CRC preserves the text", state);
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

/* Whether @p needle is anywhere in the @p cap bytes of captured output, NUL bytes (a text block's padding, printed as
   characters) included. */
static int
capture_has(const char* out, size_t cap, const char* needle) {
    const size_t n = strlen(needle);
    for (size_t i = 0U; n <= cap && i <= cap - n; i++) {
        if (memcmp(out + i, needle, n) == 0) {
            return 1;
        }
    }
    return 0;
}

/* The next transmission's first stream frame (a clean LICH chunk 0 of an empty LSF): it drops what an ended
   transmission held (m17_retire_if_ended()). */
static void
drop_ended_transmission(dsd_opts* opts, dsd_state* state) {
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t encoded[M17_LICH_BITS];
    DSD_MEMSET(lsf_bits, 0, sizeof(lsf_bits));
    build_encoded_lich_chunk(lsf_bits, 0U, encoded);
    (void)m17_process_lich(state, opts, encoded);
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

    dsd_test_capture_stderr cap;
    char out[8192];
    if (dsd_test_capture_stderr_begin(&cap, "m17_pkt_eot_text") != 0) {
        return 1;
    }
    m17_pkt_finalize_eot(opts, state, app_len, (int)(app_len + M17_PACKET_CRC_BYTES));
    (void)dsd_test_capture_stderr_end(&cap);
    (void)dsd_test_capture_stderr_read(&cap, out, sizeof(out));

    int err = 0;
    err |= expect_int("valid packet EOT starts canonical epoch", first_epoch != 0U, 1);
    /* The packet's one-block text completes. The packet's end is its transmission's end: what it held goes at the next
       transmission's first frame (its evidence stands for the EOT marker until then). */
    err |= expect_int("valid packet EOT text completes", capture_has(out, sizeof(out), "Complete: RF-PKT-OK"), 1);
    err |= expect_u8("valid packet EOT ends the transmission", state->m17_tx_ended, 1U);
    err |= expect_u8("valid packet EOT text held to the next frame", text_meta_received(state), 1U);
    drop_ended_transmission(opts, state);
    err |= expect_u8("the next frame drops the packet's text", text_meta_received(state), 0U);
    (void)expected_text;
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
    seed_text_meta(state);
    opts->aggressive_framesync = 1;
    stage_packet_with_crc(state, app, app_len, (uint16_t)(crc ^ 0x0001U));

    m17_pkt_finalize_eot(opts, state, app_len, (int)(app_len + M17_PACKET_CRC_BYTES));

    err |= expect_seeded_text_meta("bad packet CRC preserves the text", state);
    err |= expect_int("bad packet CRC clears PBC", state->m17_pbc_ct, 0);
    for (size_t i = 0U; i < total_len; i++) {
        err |= expect_u8("bad packet CRC clears packet buffer", state->m17_pkt[i], 0U);
    }

    dsd_state_ext_free_all(state);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    state->m17_can_en = -1;
    stage_packet_with_crc(state, app, app_len, (uint16_t)(crc ^ 0x0001U));

    if (dsd_test_capture_stderr_begin(&cap, "m17_pkt_relaxed_text") != 0) {
        return 1;
    }
    m17_pkt_finalize_eot(opts, state, app_len, (int)(app_len + M17_PACKET_CRC_BYTES));
    (void)dsd_test_capture_stderr_end(&cap);
    (void)dsd_test_capture_stderr_read(&cap, out, sizeof(out));

    /* -F shows the packet despite its failed CRC, and commits its text block to no assembler. */
    err |= expect_int("non-aggressive bad CRC still shows the text", capture_has(out, sizeof(out), "RF-PKT-OK"), 1);
    err |= expect_int("non-aggressive bad CRC assembles no text", capture_has(out, sizeof(out), "Complete:"), 0);
    err |= expect_u8("non-aggressive bad CRC commits no block", text_meta_received(state), 0U);
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
    /* What this station transmits is no identity received on the carrier. */
    err |= expect_int("packet LSF monitor notes no received identity", (int)state->rx_id.valid, 0);
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

/* A raw PCM16LE stdin stand-in, a temporary file holding @p count samples, read through the stdin input's one reader as
   `-i -` opens it. Returns the open file (the reader does not own its descriptor), or NULL. */
static FILE*
open_m17_stdin_input(char* path, size_t path_size, const int16_t* samples, size_t count, dsd_pcm_stream** reader) {
    *reader = NULL;
    const int fd = dsd_test_mkstemp(path, path_size, "m17_stdin");
    if (fd < 0) {
        return NULL;
    }
    int ok = 1;
    for (size_t i = 0; i < count && ok; i++) {
        const uint8_t bytes[2] = {(uint8_t)((uint16_t)samples[i] & 0xFFU), (uint8_t)((uint16_t)samples[i] >> 8)};
        ok = dsd_write(fd, bytes, sizeof bytes) == (ssize_t)sizeof bytes;
    }
    ok &= dsd_close(fd) == 0;
    FILE* file = ok ? dsd_fopen_existing_regular_file(path, "rb") : NULL;
    if (file != NULL) {
        *reader = dsd_pcm_stream_open_fd(dsd_fileno(file));
    }
    if (*reader == NULL) {
        if (file != NULL) {
            (void)fclose(file);
        }
        (void)remove(path);
        return NULL;
    }
    return file;
}

static int
test_stream_encoder_treats_stdin_eof_as_clean_shutdown(void) {
    static dsd_opts opts;
    static dsd_state state;
    char path[DSD_TEST_PATH_MAX] = {0};
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    FILE* file = open_m17_stdin_input(path, sizeof(path), NULL, 0U, &opts.audio_in_stdin);
    if (file == NULL) {
        DSD_FPRINTF(stderr, "failed to create empty M17 stdin fixture\n");
        return 1;
    }
    opts.audio_in_type = AUDIO_IN_STDIN;
    state.m17_can_en = -1;
    state.m17_rate = 8000;
    exitflag = 0;

    int err = 0;
    err |= expect_int("stream encoder treats stdin EOF as success", encodeM17STR(&opts, &state), 0);
    err |= expect_int("stream encoder closes exhausted stdin", opts.audio_in_stdin == NULL, 1);

    dsd_pcm_stream_close(opts.audio_in_stdin);
    opts.audio_in_stdin = NULL;
    exitflag = 0;
    (void)fclose(file);
    (void)remove(path);
    return err;
}

/* Issue #667 D12: the encoder reads stdin through the input's one reader, blocking as before, and gets every raw
   little-endian sample in order (at 8 kHz no filter runs, so the block is the samples themselves). */
static int
test_stream_encoder_reads_stdin_samples(void) {
    static dsd_opts opts;
    static dsd_state state;
    char path[DSD_TEST_PATH_MAX] = {0};
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    int16_t samples[160];
    for (int i = 0; i < 160; i++) {
        samples[i] = (int16_t)((i - 80) * 397);
    }
    FILE* file = open_m17_stdin_input(path, sizeof(path), samples, 160U, &opts.audio_in_stdin);
    if (file == NULL) {
        DSD_FPRINTF(stderr, "failed to create M17 stdin fixture\n");
        return 1;
    }
    opts.audio_in_type = AUDIO_IN_STDIN;
    opts.input_volume_multiplier = 1;
    m17_encoder_input in;
    m17_encoder_input_init(&in, 8000);
    short out[160];
    int err =
        expect_int("stream encoder reads a stdin block", m17_encoder_read_block(&opts, &state, &in, out, 160U), 1);
    int same = 1;
    for (int i = 0; i < 160; i++) {
        same &= out[i] == samples[i];
    }
    err |= expect_int("stream encoder reads stdin samples in order", same, 1);
    err |= expect_int("the reader stays open", opts.audio_in_stdin != NULL, 1);
    dsd_pcm_stream_close(opts.audio_in_stdin);
    opts.audio_in_stdin = NULL;
    (void)fclose(file);
    (void)remove(path);
    return err;
}

/* Write @p count copies of @p value to the pipe @p fd as little-endian samples. */
static int
pipe_put_samples(int fd, int16_t value, size_t count) {
    for (size_t i = 0; i < count; i++) {
        const uint8_t b[2] = {(uint8_t)((uint16_t)value & 0xFFU), (uint8_t)((uint16_t)value >> 8)};
        if (dsd_write(fd, b, sizeof b) != (ssize_t)sizeof b) {
            return 0;
        }
    }
    return 1;
}

/* Issue #834: a rigctl peer's confirmed retune (here a manual tune's, asked for on the decoder thread) leaves a drop of
   what the input held before it. The stream encoder reads its blocks itself, not through the symbol reader, so it
   takes the drop before its next block: the old channel's queued audio is not transmitted after the tune. */
static int
test_stream_encoder_drops_audio_queued_before_a_rigctl_retune(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    int fds[2] = {-1, -1};
#if DSD_PLATFORM_WIN_NATIVE
    const int piped = _pipe(fds, 65536U, _O_BINARY);
#else
    const int piped = pipe(fds);
#endif
    if (piped != 0) {
        DSD_FPRINTF(stderr, "failed to create the M17 encoder input pipe\n");
        return 1;
    }
    opts.audio_in_stdin = dsd_pcm_stream_open_fd(fds[0]);
    opts.audio_in_type = AUDIO_IN_STDIN;
    opts.input_volume_multiplier = 1;
    opts.use_rigctl = 1;
    int err = expect_int("the encoder's stdin reader opens", opts.audio_in_stdin != NULL, 1);
    err |= expect_int("the old channel's audio is queued", pipe_put_samples(fds[1], 7, 320U), 1);
    dsd_input_cut mark;
    dsd_input_backlog_mark(&mark); /* the leg's mark, immediately before it sends the frequency */
    err |= expect_int("the peer confirmed the tune: a drop is asked for",
                      dsd_frame_sync_request_input_drop(&opts, &state, &mark), 1);
    err |= expect_int("the new channel's audio follows", pipe_put_samples(fds[1], 9, 160U), 1);
    m17_encoder_input in;
    m17_encoder_input_init(&in, 8000);
    short out[160];
    err |= expect_int("the encoder reads a block", m17_encoder_read_block(&opts, &state, &in, out, 160U), 1);
    int new_channel = 1;
    for (int i = 0; i < 160; i++) {
        new_channel &= out[i] == 9;
    }
    err |= expect_int("the block is the new channel's audio, none of the old", new_channel, 1);
    err |= expect_int("the drop was taken", state.input_drop_pending, 0);
    dsd_pcm_stream_close(opts.audio_in_stdin);
    opts.audio_in_stdin = NULL;
    (void)dsd_close(fds[0]);
    (void)dsd_close(fds[1]);
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

static int
finalize_lsf_of_mode(dsd_opts* opts, dsd_state* state, uint16_t packet_stream, int corrupt_crc) {
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t lsf_packed[M17_LSF_BYTES];
    const unsigned long long dst = m17_encode_b40_callsign(0ULL, M17_REF_LSF_DST_CSD);
    const unsigned long long src = m17_encode_b40_callsign(0ULL, M17_REF_LSF_SRC_CSD);
    const uint16_t data_type = packet_stream != 0U ? 2U : 0U;
    build_lsf_bits(lsf_bits, dst, src, m17_compose_frame_info(packet_stream, data_type, 0U, 0U, 5U, 0U, 0U), NULL);
    const uint16_t crc = m17_attach_lsf_crc(lsf_bits, lsf_packed);
    DSD_MEMCPY(state->m17_lsf, lsf_bits, sizeof(lsf_bits));
    return m17_finalize_lsf_crc(opts, state, lsf_packed, corrupt_crc ? (uint16_t)(crc ^ 1U) : crc);
}

/* A link setup frame names its transmission's mode in TYPE bit 0, and frame sync keeps a packet sync word out of the
   stream a stream-mode LSF opened. Only an LSF whose CRC passed says so, and each LSF is read afresh, so a packet
   transmission behind its own LSF is taken (issue #834). */
static int
test_lsf_names_stream_mode_only_behind_its_crc(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->aggressive_framesync = 1;

    int err = 0;
    err |= expect_int("stream LSF passes", finalize_lsf_of_mode(opts, state, 1U, 0), 0);
    err |= expect_u8("a stream LSF names stream mode", state->m17_lsf_stream_mode, 1U);
    err |= expect_int("packet LSF passes", finalize_lsf_of_mode(opts, state, 0U, 0), 0);
    err |= expect_u8("the next LSF, a packet one, clears it", state->m17_lsf_stream_mode, 0U);
    (void)finalize_lsf_of_mode(opts, state, 1U, 0);
    err |= expect_int("damaged stream LSF fails", finalize_lsf_of_mode(opts, state, 1U, 1), 1);
    err |= expect_u8("an LSF whose CRC failed names nothing", state->m17_lsf_stream_mode, 0U);
    dsd_state_ext_free_all(state);
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

/* What the signal search's confirmation can read when the evidence hook fires. The app-control observer copies the
 * active call's identity and access code inside the hook, from state, and only on that confirmation. */
static struct {
    int calls;
    char protocol[16];
    int call_active;
    int call_protocol_matches;
    char source_text[DSD_CALL_IDENTITY_TEXT_SIZE];
    char target_text[DSD_CALL_IDENTITY_TEXT_SIZE];
    int access_valid;
    uint8_t access_kind;
    uint16_t access_value;
} g_search_evidence;

static void
capture_search_evidence(const dsd_state* state, const char* protocol, int sync, int slot, uint32_t target,
                        uint32_t source, const char* access) {
    (void)sync;
    (void)slot;
    (void)target;
    (void)source;
    (void)access;
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof(call));
    g_search_evidence.calls++;
    DSD_SNPRINTF(g_search_evidence.protocol, sizeof(g_search_evidence.protocol), "%s", protocol ? protocol : "");
    const int active = dsd_call_state_get(state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE;
    g_search_evidence.call_active = active;
    g_search_evidence.call_protocol_matches = active && call.protocol == state->synctype;
    DSD_SNPRINTF(g_search_evidence.source_text, sizeof(g_search_evidence.source_text), "%s",
                 active ? call.source_text : "");
    DSD_SNPRINTF(g_search_evidence.target_text, sizeof(g_search_evidence.target_text), "%s",
                 active ? call.target_text : "");
    g_search_evidence.access_valid = dsd_access_code_current(
        state, state->synctype, active ? call.service_options : 0U, active ? call.has_service_metadata : 0U,
        &g_search_evidence.access_kind, &g_search_evidence.access_value);
}

/* Call notes a signal search hears, and the source the slot-0 call record named when each arrived. */
static int g_call_notes;
static char g_call_note_source[64];

static void
capture_call_note(const dsd_state* state, uint8_t slot) {
    dsd_call_snapshot call;
    g_call_notes++;
    g_call_note_source[0] = '\0';
    if (slot == 0U && dsd_call_state_get(state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE) {
        DSD_SNPRINTF(g_call_note_source, sizeof(g_call_note_source), "%s", call.source_text);
    }
}

/* The first CRC-valid LSF is enough for its callsigns and CAN to reach a signal search: the search's confirmation is
 * reported once the LSF's call is published, not before it, and a damaged LSF reports nothing. */
static int
test_lsf_search_evidence_follows_the_published_call(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t lsf_packed[M17_LSF_BYTES];

    dsd_state_ext_free_all(state);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMSET(&g_search_evidence, 0, sizeof(g_search_evidence));
    const unsigned long long dst = m17_encode_b40_callsign(0ULL, M17_REF_LSF_DST_CSD);
    const unsigned long long src = m17_encode_b40_callsign(0ULL, M17_REF_LSF_SRC_CSD);
    const uint16_t type_word = m17_compose_frame_info(1U, 2U, 0U, 0U, 7U, 0U, 0U);
    build_lsf_bits(lsf_bits, dst, src, type_word, NULL);
    const uint16_t crc = m17_attach_lsf_crc(lsf_bits, lsf_packed);
    state->synctype = DSD_SYNC_M17_LSF_POS;
    dsd_signal_search_evidence_set(capture_search_evidence);
    dsd_signal_search_call_set(capture_call_note);
    g_call_notes = 0;

    int err = 0;
    opts->aggressive_framesync = 1;
    DSD_MEMCPY(state->m17_lsf, lsf_bits, sizeof(lsf_bits));
    err |= expect_int("strict bad LSF CRC", m17_finalize_lsf_crc(opts, state, lsf_packed, crc ^ 1U), 1);
    opts->aggressive_framesync = 0;
    DSD_MEMCPY(state->m17_lsf, lsf_bits, sizeof(lsf_bits));
    err |= expect_int("relaxed bad LSF CRC", m17_finalize_lsf_crc(opts, state, lsf_packed, crc ^ 1U), 1);
    err |= expect_int("a failing LSF CRC reports nothing to search", g_search_evidence.calls, 0);
    err |= expect_int("a failing LSF CRC notes no call", g_call_notes, 0);

    opts->aggressive_framesync = 1;
    DSD_MEMCPY(state->m17_lsf, lsf_bits, sizeof(lsf_bits));
    err |= expect_int("valid LSF CRC", m17_finalize_lsf_crc(opts, state, lsf_packed, crc), 0);
    err |= expect_int("a valid LSF confirms to search once", g_search_evidence.calls, 1);
    err |= expect_int("search evidence names M17", strcmp(g_search_evidence.protocol, "M17"), 0);
    err |= expect_int("the LSF's call is published before search hears", g_search_evidence.call_active, 1);
    err |= expect_int("the published call is this frame's protocol", g_search_evidence.call_protocol_matches, 1);
    err |= expect_int("search sees the LSF source", strcmp(g_search_evidence.source_text, M17_REF_LSF_SRC_CSD), 0);
    err |= expect_int("search sees the LSF destination", strcmp(g_search_evidence.target_text, M17_REF_LSF_DST_CSD), 0);
    err |= expect_int("search sees the LSF CAN", g_search_evidence.access_valid, 1);
    err |= expect_u8("search CAN kind", g_search_evidence.access_kind, (uint8_t)DSD_ACCESS_CODE_CAN);
    err |= expect_int("search CAN value", g_search_evidence.access_value, 7);
    err |= expect_int("the LSF's call is noted once", g_call_notes, 1);
    err |= expect_int("the noted call names the LSF source", strcmp(g_call_note_source, M17_REF_LSF_SRC_CSD), 0);

    dsd_signal_search_evidence_set(NULL);
    dsd_signal_search_call_set(NULL);
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
    m17_confirm_note_evidence(NULL, state, M17_EVIDENCE_WEAK);
    m17_confirm_end_frame(state);
    err |= expect_int("one clean LICH does not open a call",
                      m17_dispatch_stream_payload(opts, state, payload_bits, M17_REF_STREAM_FN, processed_bits),
                      M17_STREAM_CLEAR_DISPATCHED);
    err |= expect_int("one clean LICH publishes no call", get_m17_call(state, &call), 0);

    m17_confirm_begin_frame(state);
    m17_confirm_note_evidence(NULL, state, M17_EVIDENCE_WEAK);
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
        m17_voice_chain_process(&opts, &state, &chain, voice, nsam, 1);
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
        m17_voice_chain_process(&opts, &state, &chain, voice, 160U, 1);
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
    dsd_net_audio_input_hooks_set(&(dsd_net_audio_input_hooks){.udp_read_sample = fake_udp_tone_read});
    const double level = encoder_read_db(&opts, &state, rate_hz, amplitude, peak);
    dsd_net_audio_input_hooks_set(NULL);
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
    dsd_net_audio_input_hooks_set(&hooks);
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
    dsd_runtime_set_controls_pending(NULL);
    /* A frontend publication a P25 watchdog tick put off gives the block up the same way, with nothing queued: the
       decoder's pump publishes it, and the encoder reads the block again. */
    dsd_runtime_set_publish_pending(fake_controls_pending);
    g_silent_waits = 0;
    if (m17_encoder_read_block(&opts, &state, &in, out, 160U) != 2) {
        DSD_FPRINTF(stderr, "silent udp: the block was not given up for the put-off publication\n");
        rc = 1;
    }
    dsd_runtime_set_publish_pending(NULL);
    g_silent_pending = 0;
    dsd_net_audio_input_hooks_set(NULL);
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
    dsd_rtl_stream_io_hooks_set(&(dsd_rtl_stream_io_hooks){
        .read = fake_rtl_tone_read,
        .return_pwr = fake_rtl_return_pwr,
    });
    double peak = 0.0;
    const double level = encoder_read_db(&opts, &state, 48000, 8231.0, &peak);
    dsd_rtl_stream_io_hooks_set(NULL);
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

/* Issue #518 D5: the stream the encoder reads, sample @p n of it a function of n alone; the gap sits before sample
   g_gap_at, and the stream's count moves before that sample can be read. */
static uint64_t g_gap_next = 0U;
static uint64_t g_gap_at = UINT64_MAX;

static int
fake_rtl_gap_read(void* rtl_ctx, float* out, size_t count, int* out_got) {
    (void)rtl_ctx;
    if (out == NULL || out_got == NULL || count != 1U) {
        return -1;
    }
    out[0] = 0.1f * (float)sin(0.05 * (double)g_gap_next) + 0.02f * (float)(g_gap_next % 7U);
    g_gap_next++;
    *out_got = 1;
    return 0;
}

static uint64_t
fake_rtl_gap_seq(void) {
    return g_gap_next > g_gap_at ? 1U : 0U;
}

/* A gap in the stream the M17 encoder reads (a USB recovery, an rtl_tcp reconnect): the encoder reads the stream
   itself, so it takes the gap up there. The 40 ms block it was filling is dropped (M17_STR_READ_AGAIN, nothing
   encoded from it) and its input filter starts over: the next block is exactly what a fresh input reads from the
   samples after the gap. The symbol reader is left nothing to raise. */
static int
test_m17_encoder_gap_drops_partial_block(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    assert(opts != NULL);
    dsd_state* state = calloc(1, sizeof(*state));
    assert(state != NULL);
    dsd_state* fresh_state = calloc(1, sizeof(*fresh_state));
    assert(fresh_state != NULL);
    static int token;
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtl_volume_multiplier = 1;
    state->rtl_ctx = (struct RtlSdrContext*)&token;
    dsd_rtl_stream_io_hooks_set(&(dsd_rtl_stream_io_hooks){
        .read = fake_rtl_gap_read,
        .return_pwr = fake_rtl_return_pwr,
    });
    static const dsd_rtl_stream_metrics_hooks hooks = {.discontinuity_seq = fake_rtl_gap_seq};
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    g_gap_next = 0U;
    g_gap_at = UINT64_MAX;
    m17_encoder_input in;
    m17_encoder_input_init(&in, 48000);
    short out[160];
    int rc = 0;
    rc |= expect_int("gap: a block before the gap reads", m17_encoder_read_block(opts, state, &in, out, 160U), 1);
    /* The gap, part way into the next block. */
    g_gap_at = g_gap_next + 300U;
    rc |= expect_int("gap: the block the gap cuts is given up", m17_encoder_read_block(opts, state, &in, out, 160U), 2);
    rc |= expect_int("gap: the read that met it was the last", (int)(g_gap_next - g_gap_at), 1);
    rc |= expect_int("gap: the encoder took the gap up", (int)state->rtl_stream_gap_seq, 1);
    rc |= expect_int("gap: no interruption is left for the symbol reader", state->input_interrupted, 0);
    const uint64_t resume_at = g_gap_next;
    rc |= expect_int("gap: the next block reads", m17_encoder_read_block(opts, state, &in, out, 160U), 1);

    /* A fresh input over the same samples after the gap. */
    DSD_MEMSET(fresh_state, 0, sizeof(*fresh_state));
    fresh_state->rtl_ctx = (struct RtlSdrContext*)&token;
    fresh_state->rtl_stream_gap_seq = 1U;
    m17_encoder_input fresh;
    m17_encoder_input_init(&fresh, 48000);
    short want[160];
    g_gap_next = resume_at;
    rc |= expect_int("gap: the fresh input reads", m17_encoder_read_block(opts, fresh_state, &fresh, want, 160U), 1);
    rc |= expect_int("gap: the block after the gap starts from a fresh filter", memcmp(out, want, sizeof out) == 0, 1);

    dsd_rtl_stream_metrics_hooks_set(NULL);
    dsd_rtl_stream_io_hooks_set(NULL);
    free(fresh_state);
    free(state);
    free(opts);
    return rc;
}
#endif

#ifdef USE_RADIO
/* Issue #518 D4: the stream yields (DSD_RTL_READ_YIELD) at read g_yield_at, as a live read with no stream does when a
   command is queued; a read that yields hands out no sample. */
static uint64_t g_yield_at = UINT64_MAX;
static int g_yield_reads = 0;

static int
fake_rtl_yield_read(void* rtl_ctx, float* out, size_t count, int* out_got) {
    if (out_got != NULL) {
        *out_got = 0;
    }
    if (g_yield_reads++ == (int)g_yield_at) {
        return DSD_RTL_READ_YIELD;
    }
    return fake_rtl_gap_read(rtl_ctx, out, count, out_got);
}

/* m17_encoder_survives_yield: the M17 encoder reads the stream itself, so a yield reaches it directly. It is no
   failure: the encoder asks for no shutdown, gives the block up as on a gap (M17_STR_READ_AGAIN), so its loop pumps the
   command before it reads again, and reads on from the next sample. */
static int
test_m17_encoder_survives_yield(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    assert(opts != NULL);
    dsd_state* state = calloc(1, sizeof(*state));
    assert(state != NULL);
    static int token;
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtl_volume_multiplier = 1;
    state->rtl_ctx = (struct RtlSdrContext*)&token;
    dsd_rtl_stream_io_hooks_set(&(dsd_rtl_stream_io_hooks){
        .read = fake_rtl_yield_read,
        .return_pwr = fake_rtl_return_pwr,
    });
    g_gap_next = 0U;
    g_gap_at = UINT64_MAX;
    g_yield_reads = 0;
    g_yield_at = 960U + 300U;
    dsd_exitflag_store(0);
    m17_encoder_input in;
    m17_encoder_input_init(&in, 48000);
    short out[160];
    int rc = 0;
    rc |= expect_int("yield: a block before it reads", m17_encoder_read_block(opts, state, &in, out, 160U), 1);
    rc |= expect_int("yield: the block it cuts is given up for the command",
                     m17_encoder_read_block(opts, state, &in, out, 160U), 2);
    rc |= expect_int("yield: no shutdown", dsd_exitflag_load(), 0);
    rc |= expect_int("yield: no interruption is left for the symbol reader", state->input_interrupted, 0);
    const uint64_t next = g_gap_next;
    rc |= expect_int("yield: the next block reads", m17_encoder_read_block(opts, state, &in, out, 160U), 1);
    rc |= expect_int("yield: it reads on from the next sample", (int)(g_gap_next - next), 960);
    rc |= expect_int("yield: still no shutdown", dsd_exitflag_load(), 0);
    g_yield_at = UINT64_MAX;
    dsd_exitflag_store(0);
    dsd_rtl_stream_io_hooks_set(NULL);
    free(state);
    free(opts);
    return rc;
}
#endif

#ifdef DSD_NEO_TEST_AUDIO_WRAP
/* The local output's writes ('W', 'Z' for all zeros) and drains ('D') in order, while g_audio_wrap_on. */
static char g_audio_events[2048];
static int g_audio_event_count = 0;
static int g_audio_wrap_on = 0;
/* The channel count of the local stream under the wrap, and the last block it took: a backend reads frames x channels
   samples from the buffer it is handed (issue #658), so the wrap copies that many. */
static int g_audio_wrap_channels = 1;
static size_t g_audio_wrap_last_frames = 0U;
static int16_t g_audio_wrap_last[320U * 2U];

// GNU linker wrappers of the local output (-Wl,--wrap): the names are the linker's.
// NOLINTBEGIN(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
int __real_dsd_audio_write(dsd_audio_stream* stream, const int16_t* buffer, size_t frames);
int __real_dsd_audio_drain(dsd_audio_stream* stream);
int __wrap_dsd_audio_write(dsd_audio_stream* stream, const int16_t* buffer, size_t frames);
int __wrap_dsd_audio_drain(dsd_audio_stream* stream);
int __real_dsd_audio_output_health_get(dsd_audio_stream* stream, dsd_audio_output_health* out);
int __wrap_dsd_audio_output_health_get(dsd_audio_stream* stream, dsd_audio_output_health* out);

/* The audio sinks ask each write's stream for its device health (issue #659); the wrapped output never fails. */
int
__wrap_dsd_audio_output_health_get(dsd_audio_stream* stream, dsd_audio_output_health* out) {
    if (!g_audio_wrap_on) {
        return __real_dsd_audio_output_health_get(stream, out);
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    return 0;
}

int
__wrap_dsd_audio_write(dsd_audio_stream* stream, const int16_t* buffer, size_t frames) {
    if (!g_audio_wrap_on) {
        return __real_dsd_audio_write(stream, buffer, frames);
    }
    g_audio_wrap_last_frames = frames;
    const size_t samples = frames * (size_t)g_audio_wrap_channels;
    if (buffer != NULL && samples <= sizeof(g_audio_wrap_last) / sizeof(g_audio_wrap_last[0])) {
        DSD_MEMCPY(g_audio_wrap_last, buffer, samples * sizeof(buffer[0]));
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
   raw fd, for the 3200 payload (two codec frames) and the 1600 payload (one codec frame, played as two 160-sample
   frames, issue #658). */
static int
test_stream_voice_audio_honors_mute_on_every_output(void) {
    static const struct {
        const char* tag;
        uint8_t dt;
        int blocks;
        size_t block_bytes;
    } formats[] = {
        {"3200", 2U, 2, 160U * sizeof(short)},
        {"1600", 3U, 2, 160U * sizeof(short)},
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
                prepare_voice_output(opts, state);
                g_codec2_decode_samples = formats[f].dt == 3U ? 320U : 160U;
                opts->slot1_on = 1;
                opts->audio_out = audio_out;
                opts->audio_out_type = out_type;
                state->m17_str_dt = formats[f].dt;
                state->m17_can_en = -1;
                m17_confirm_note_evidence(NULL, state, M17_EVIDENCE_STRONG);

                char path[DSD_TEST_PATH_MAX];
                int fd = -1;
                if (out_type == 1) {
                    fd = dsd_test_mkstemp(path, sizeof(path), "dsdneo_m17_mute");
                    if (fd < 0) {
                        DSD_FPRINTF(stderr, "m17 mute: could not create the raw output temp file\n");
                        err |= 1;
                        dsd_state_ext_free_all(state);
                        continue;
                    }
                    opts->audio_out_fd = fd;
                }
#ifdef DSD_NEO_TEST_AUDIO_WRAP
                g_audio_event_count = 0;
                g_audio_events[0] = '\0';
                g_audio_wrap_on = out_type == 0;
                /* The local stream is open (a sentinel the wraps take): the sink writes no stream that is not. */
                opts->audio_out_stream = out_type == 0 ? (dsd_audio_stream*)&g_audio_wrap_on : NULL;
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

#ifdef USE_CODEC2
/* Issue #574: Codec2 voice stamps audible audio whenever a frame it decoded is written, the first frame of a
   transmission included, for both payloads (the 3200 pair and the 1600 single) and on every output type. A muted
   output, slot 1 switched off, a CAN the filter rejects or an encrypted stream with no key stamps nothing. */
static int
test_stream_voice_audio_stamps_what_it_plays(void) {
    static const struct {
        const char* tag;
        uint8_t dt;
        int blocks;
        size_t block_bytes;
    } formats[] = {
        {"3200", 2U, 2, 160U * sizeof(short)},
        {"1600", 3U, 2, 160U * sizeof(short)},
    };

    static const int out_types[] = {
#ifdef DSD_NEO_TEST_AUDIO_WRAP
        0,
#endif
        1,
        8,
    };
    static const struct {
        const char* tag;
        int audio_out;
        int slot1_on;
        int can_filtered;
        int keyless;
        int plays;
    } cases[] = {
        {"first frame", 1, 1, 0, 0, 1},           {"output muted", 0, 1, 0, 0, 0},
        {"slot 1 switched off", 1, 0, 0, 0, 0},   {"CAN filtered", 1, 1, 1, 0, 0},
        {"encrypted with no key", 1, 1, 0, 1, 0},
    };

    static const uint8_t payload_bytes[16] = {0x60U, 0x61U, 0x62U, 0x63U, 0x64U, 0x65U, 0x66U, 0x67U,
                                              0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U};
    uint8_t payload_bits[128];
    uint8_t processed_bits[128];
    bytes_to_bits(payload_bytes, payload_bits, sizeof(payload_bytes));

    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    int err = 0;
    dsd_audio_activity_arm();
    for (size_t f = 0U; f < sizeof(formats) / sizeof(formats[0]); f++) {
        for (size_t o = 0U; o < sizeof(out_types) / sizeof(out_types[0]); o++) {
            for (size_t c = 0U; c < sizeof(cases) / sizeof(cases[0]); c++) {
                const int out_type = out_types[o];
                DSD_MEMSET(opts, 0, sizeof(*opts));
                DSD_MEMSET(state, 0, sizeof(*state));
                reset_audio_fakes();
                install_fake_udp_audio();
                prepare_voice_output(opts, state);
                g_codec2_decode_samples = formats[f].dt == 3U ? 320U : 160U;
                opts->slot1_on = cases[c].slot1_on;
                opts->audio_out = cases[c].audio_out;
                opts->audio_out_type = out_type;
                state->m17_str_dt = formats[f].dt;
                state->m17_can_en = cases[c].can_filtered ? 5 : -1;
                state->m17_can = 3U;
                state->m17_enc = cases[c].keyless ? 2U : 0U;
                m17_confirm_note_evidence(NULL, state, M17_EVIDENCE_STRONG);

                char path[DSD_TEST_PATH_MAX];
                int fd = -1;
                if (out_type == 1) {
                    fd = dsd_test_mkstemp(path, sizeof(path), "dsdneo_m17_stamp");
                    if (fd < 0) {
                        DSD_FPRINTF(stderr, "m17 stamp: could not create the raw output temp file\n");
                        err |= 1;
                        dsd_state_ext_free_all(state);
                        continue;
                    }
                    opts->audio_out_fd = fd;
                }
#ifdef DSD_NEO_TEST_AUDIO_WRAP
                g_audio_event_count = 0;
                g_audio_events[0] = '\0';
                g_audio_wrap_on = out_type == 0;
                /* The local stream is open; the wrap takes its writes. */
                opts->audio_out_stream = out_type == 0 ? (dsd_audio_stream*)&g_audio_wrap_on : NULL;
#endif
                dsd_audio_activity_reset();

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
                uint64_t stamp = 0U;
                dsd_audio_activity_read(&stamp, NULL);
                char label[128];
                DSD_SNPRINTF(label, sizeof(label), "m17 %s %s on output %d: blocks", formats[f].tag, cases[c].tag,
                             out_type);
                err |= expect_int(label, written, cases[c].plays ? formats[f].blocks : 0);
                DSD_SNPRINTF(label, sizeof(label), "m17 %s %s on output %d: stamp", formats[f].tag, cases[c].tag,
                             out_type);
                err |= expect_int(label, stamp != 0U, cases[c].plays);
                reset_audio_fakes();
                dsd_state_ext_free_all(state);
            }
        }
    }
    dsd_audio_activity_reset();
    return err;
}
#endif

#ifdef USE_CODEC2
/* Issue #658: decoded Codec2 voice takes the stream's format. Each payload's frames (two 160-sample codec frames at
   3200, one 320-sample codec frame at 1600) go out as 160-frame blocks of the stream's channel count, s16 by default
   and f32 under -y, the channels carrying the same frame; the fixed gain -g applies. On a two-channel local stream the
   backend reads 320 samples from each block it is handed, all of them the decoded frame's. */
static int
run_voice_format_case(uint8_t dt, int floating, int channels, int out_type, float gain, int16_t* first_block,
                      size_t* first_block_count) {
    static const uint8_t payload_bytes[16] = {0x70U, 0x71U, 0x72U, 0x73U, 0x74U, 0x75U, 0x76U, 0x77U,
                                              0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U};
    uint8_t payload_bits[128];
    uint8_t processed_bits[128];
    bytes_to_bits(payload_bytes, payload_bits, sizeof(payload_bytes));
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    reset_audio_fakes();
    install_fake_udp_audio();
    prepare_voice_output(opts, state);
    g_codec2_decode_samples = dt == 3U ? 320U : 160U;
    opts->slot1_on = 1;
    opts->audio_out = 1;
    opts->audio_out_type = out_type;
    opts->floating_point = floating;
    opts->pulse_digi_out_channels = channels;
    opts->audio_gain = gain;
    state->m17_str_dt = dt;
    state->m17_can_en = -1;
    m17_confirm_note_evidence(NULL, state, M17_EVIDENCE_STRONG);
#ifdef DSD_NEO_TEST_AUDIO_WRAP
    g_audio_event_count = 0;
    g_audio_events[0] = '\0';
    g_audio_wrap_on = out_type == 0;
    g_audio_wrap_channels = channels;
    g_audio_wrap_last_frames = 0U;
    DSD_MEMSET(g_audio_wrap_last, 0, sizeof(g_audio_wrap_last));
    opts->audio_out_stream = out_type == 0 ? (dsd_audio_stream*)&g_audio_wrap_on : NULL;
#endif

    (void)m17_dispatch_stream_payload(opts, state, payload_bits, M17_REF_STREAM_FN, processed_bits);

    int err = 0;
    char label[128];
    const size_t count = 160U * (size_t)channels;
    DSD_SNPRINTF(label, sizeof(label), "m17 dt %u float %d ch %d out %d", (unsigned)dt, floating, channels, out_type);
    if (out_type == 8) {
        err |= expect_int(label, g_udp_audio_calls, 2);
        err |= expect_int("m17 voice block bytes", (int)g_udp_audio_last_nsam,
                          (int)(count * (floating ? sizeof(float) : sizeof(short))));
        for (size_t i = 0U; i < count; i++) {
            const int source = dt == 3U ? 1001 + 160 + (int)(i / (size_t)channels) : 1002 + (int)(i / (size_t)channels);
            const short want = (short)((float)source * gain);
            if (floating) {
                float sample;
                DSD_MEMCPY(&sample, g_udp_audio_last_data + (i * sizeof(float)), sizeof(sample));
                err |= expect_int("m17 f32 voice matches chained short", fabsf(sample - (float)want / 32768.0f) < 1e-9f,
                                  1);
            } else {
                short sample;
                DSD_MEMCPY(&sample, g_udp_audio_last_data + (i * sizeof(short)), sizeof(sample));
                err |= expect_int("m17 s16 voice matches chained short", sample, want);
                if (first_block != NULL && i < 320U) {
                    first_block[i] = sample;
                }
            }
        }
        if (first_block_count != NULL) {
            *first_block_count = floating ? 0U : count;
        }
    }
#ifdef DSD_NEO_TEST_AUDIO_WRAP
    if (out_type == 0) {
        err |= expect_int(label, g_audio_event_count, 2);
        err |= expect_int("m17 voice local stream frames", (int)g_audio_wrap_last_frames, 160);
        for (size_t i = 0U; i < count; i++) {
            /* The second block holds the decoded frame from sample 160 of a 1600 frame, or the second 3200 frame. */
            const int want = dt == 3U ? 1001 + 160 + (int)(i / (size_t)channels) : 1002 + (int)(i / (size_t)channels);
            err |= expect_int("m17 voice local stream sample", g_audio_wrap_last[i], (short)((float)want * gain));
        }
        g_audio_wrap_on = 0;
        g_audio_wrap_channels = 1;
    }
#endif
    reset_audio_fakes();
    dsd_state_ext_free_all(state);
    return err;
}

/* What M17's Codec2 voice tells the voice chain: received voice, which reaches playback only confirmed, decrypted and
   CAN-allowed, is decodable, admitted speech; the encoder's own transmission, decoded back for its monitor, is the
   monitor's (DSD_VOICE_MEDIA_TX_MONITOR), so it never passes for received speech. */
static int g_m17_info_frames;
static int g_m17_info_received;
static int g_m17_info_tx_monitor;

static void
m17_info_observe(void* ctx, int slot, const float* frame, const dsd_voice_frame_info* info) {
    (void)ctx;
    (void)frame;
    g_m17_info_frames++;
    if (!info) {
        return;
    }
    if (slot == 0 && info->media == DSD_VOICE_MEDIA_TX_MONITOR && info->talkspurt == 0U) {
        g_m17_info_tx_monitor++;
    } else if (slot == 0 && info->media == DSD_VOICE_MEDIA_SPEECH && info->decodable == 1U && info->admitted == 1U
               && info->talkspurt == 0U) {
        g_m17_info_received++;
    }
}

static void
m17_info_reset(void) {
    g_m17_info_frames = 0;
    g_m17_info_tx_monitor = 0;
    g_m17_info_received = 0;
}

static int
test_stream_voice_tells_the_chain_received_speech(void) {
    int err = 0;
    m17_info_reset();
    dsd_voice_chain_set_observer_for_test(m17_info_observe, NULL);
    err |= run_voice_format_case(2U, 0, 1, 8, 1.0f, NULL, NULL);
    err |= run_voice_format_case(3U, 0, 1, 8, 1.0f, NULL, NULL);
    dsd_voice_chain_set_observer_for_test(NULL, NULL);
    err |= expect_int("m17 received voice: two frames per payload, each received speech",
                      g_m17_info_frames == 4 && g_m17_info_received == 4, 1);
    return err;
}

/* The stream encoder (-fZ) with its decoded monitor: the voice it decodes back from its own transmission runs the
   chain as the monitor's (DSD_VOICE_MEDIA_TX_MONITOR) and plays at the level codec2 decoded it at, under a fixed -g 3 as
   under auto (issue #518 B17); the receive slot's speech AGC is left exactly as it started. */
/* Whether @p a and @p b hold the same bits, read as 32-bit words. An untouched AGC is one whose values are still the
   very bits init set, so this is the claim, with no float compare. The AGC's learned block and its gains are 4-byte
   fields only, so the words are their values' bits, with no padding between them. */
static int
same_words(const void* a, const void* b, size_t size) {
    uint32_t wa[128] = {0};
    uint32_t wb[128] = {0};
    if (size % sizeof(uint32_t) != 0U || size > sizeof wa) {
        return 0;
    }
    DSD_MEMCPY(wa, a, size);
    DSD_MEMCPY(wb, b, size);
    for (size_t i = 0U; i < size / sizeof(uint32_t); i++) {
        if (wa[i] != wb[i]) {
            return 0;
        }
    }
    return 1;
}

static int
monitor_run(float gain) {
    static dsd_opts opts;
    static dsd_state state;
    static int16_t samples[160U * 24U];
    /* Room for every frame the monitor stages: the encoder's frames and the trailing ones it flushes. */
    static short history[100U + (160U * 256U)];
    char path[DSD_TEST_PATH_MAX] = {0};
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        samples[i] = (int16_t)(8000.0 * sin(2.0 * 3.14159265358979 * 440.0 * (double)i / 8000.0));
    }
    FILE* file =
        open_m17_stdin_input(path, sizeof(path), samples, sizeof(samples) / sizeof(samples[0]), &opts.audio_in_stdin);
    if (file == NULL) {
        DSD_FPRINTF(stderr, "failed to create M17 stdin fixture\n");
        return 1;
    }
    reset_audio_fakes();
    install_fake_udp_audio();
    prepare_voice_output(&opts, &state);
    state.audio_out_buf = history;
    state.audio_out_buf_p = history + 100;
    opts.audio_gain = gain;
    opts.audio_in_type = AUDIO_IN_STDIN;
    opts.input_volume_multiplier = 1;
    opts.monitor_input_audio = 0;
    opts.audio_out = 1;
    opts.audio_out_type = 8;
    opts.slot1_on = 1;
    state.m17_can_en = -1;
    state.m17_rate = 8000;
    state.m17encoder_tx = 1;
    exitflag = 0;
    m17_info_reset();
    dsd_voice_chain_set_observer_for_test(m17_info_observe, NULL);
    (void)encodeM17STR(&opts, &state);
    dsd_voice_chain_set_observer_for_test(NULL, NULL);
    int err = expect_int("m17 encoder monitor: decoded voice ran the chain", g_m17_info_frames > 0, 1);
    err |= expect_int("m17 encoder monitor: within the staging room",
                      state.audio_out_buf_p <= history + (sizeof(history) / sizeof(history[0])), 1);
    err |= expect_int("m17 encoder monitor: every frame the monitor's", g_m17_info_tx_monitor, g_m17_info_frames);
    /* The last block played is the last frame the fake decoded: 1000 + its call count + i, as decoded. */
    err |= expect_int("m17 encoder monitor: the last block is one frame", (int)g_udp_audio_last_nsam,
                      (int)(160U * sizeof(short)));
    int unity = 1;
    for (size_t i = 0U; i < 160U; i++) {
        short sample = 0;
        DSD_MEMCPY(&sample, g_udp_audio_last_data + (i * sizeof(short)), sizeof(sample));
        unity &= sample == (short)(1000 + g_codec2_decode_calls + (int)i);
    }
    err |= expect_int("m17 encoder monitor: plays the decoded level whatever -g says", unity, 1);
    dsd_speech_agc fresh;
    DSD_MEMSET(&fresh, 0, sizeof fresh);
    (void)dsd_speech_agc_init(&fresh, 8000, DSD_VOICE_LEVEL_START_GAIN_DB, DSD_SPEECH_AGC_TARGET_DBFS,
                              DSD_SPEECH_AGC_CEILING_DBFS);
    const dsd_speech_agc* slot0 = dsd_voice_level_agc(&state, 0);
    /* Untouched: no sample run, and the learned state, the gain and the limiter bit for bit what init set. */
    const int untouched = slot0 == NULL
                          || (slot0->frames == 0U && slot0->frame_pos == 0
                              && same_words(&slot0->learned, &fresh.learned, sizeof fresh.learned)
                              && same_words(&slot0->g_cur, &fresh.g_cur, sizeof fresh.g_cur)
                              && same_words(&slot0->g_lim, &fresh.g_lim, sizeof fresh.g_lim));
    err |= expect_int("m17 encoder monitor: the receive slot's AGC untouched", untouched, 1);
    dsd_pcm_stream_close(opts.audio_in_stdin);
    opts.audio_in_stdin = NULL;
    exitflag = 0;
    (void)fclose(file);
    (void)remove(path);
    reset_audio_fakes();
    dsd_state_ext_free_all(&state);
    return err;
}

static int
test_stream_encoder_monitor_plays_what_it_sends(void) {
    return monitor_run(3.0f) | monitor_run(0.0f);
}

/* Issue #518 B17: the stream encoder's microphone through its key-ups, end to end. A script of 40 ms reads (320
   samples: two codec2 frames at 3200 bit/s, one at 1600) feeds the UDP input a 1 kHz tone and noise, and works the TX
   toggle as its command does between reads, or leaves keying to the VOX; the fake codec2 logs the level of every frame
   the encoder hands it. */
enum { MIC_ITER_SAMPLES = 320, MIC_MAX_ITERS = 160 };

typedef struct {
    int iters;    /* 40 ms reads */
    int ptt;      /* 1 keyed, 0 unkeyed (the TX toggle), -1 left to the VOX */
    double tone;  /* 1 kHz tone amplitude, int16 units */
    double noise; /* uniform noise amplitude, int16 units */
} mic_segment;

static const mic_segment* g_mic_script;
static size_t g_mic_script_len;
static long g_mic_reads;
static dsd_state* g_mic_state;
static double g_mic_phase;
static uint64_t g_mic_noise;
/* As each read starts: m17encoder_tx (what the last frame decided) and the number of codec2 frames before it. */
static int g_mic_tx[MIC_MAX_ITERS];
static int g_mic_frame_at[MIC_MAX_ITERS + 1];
static long g_mic_iters;

static const mic_segment*
mic_segment_at(long iter) {
    for (size_t s = 0U; s < g_mic_script_len; s++) {
        if (iter < g_mic_script[s].iters) {
            return &g_mic_script[s];
        }
        iter -= g_mic_script[s].iters;
    }
    return NULL;
}

static double
mic_noise_sample(void) {
    g_mic_noise ^= g_mic_noise >> 12;
    g_mic_noise ^= g_mic_noise << 25;
    g_mic_noise ^= g_mic_noise >> 27;
    const uint64_t r = g_mic_noise * 2685821657736338717ULL;
    return ((double)(r >> 11) / 4503599627370496.0) - 1.0;
}

/* What the TX toggle command does (apply_cmd_m17_tx_toggle()), before the read it applies to. */
static void
mic_apply_ptt(int ptt) {
    if (ptt < 0) {
        return;
    }
    if (ptt == 0 && g_mic_state->m17encoder_tx == 1) {
        g_mic_state->m17encoder_eot = 1;
    }
    g_mic_state->m17encoder_tx = ptt;
}

static int
fake_udp_mic_read(dsd_opts* opts, int16_t* out) {
    (void)opts;
    const long iter = g_mic_reads / MIC_ITER_SAMPLES;
    const mic_segment* seg = mic_segment_at(iter);
    if (seg == NULL || out == NULL || iter >= MIC_MAX_ITERS) {
        return 0;
    }
    if ((g_mic_reads % MIC_ITER_SAMPLES) == 0) {
        g_mic_tx[iter] = g_mic_state->m17encoder_tx;
        g_mic_frame_at[iter] = g_codec2_encode_count;
        g_mic_iters = iter + 1;
        mic_apply_ptt(seg->ptt);
    }
    g_mic_reads++;
    const double v = (seg->tone * sin(g_mic_phase)) + (seg->noise * mic_noise_sample());
    g_mic_phase += 2.0 * 3.14159265358979323846 * 1000.0 / 8000.0;
    *out = (int16_t)lrint(v);
    return 1;
}

/* A fresh encoder setup at 3200 (@p dt 2) or 1600 bit/s (3), on auto gain with no filters, keyed by the script's PTT or,
   with @p vox, by the VOX on a level squelch at -40 dBFS. */
static void
mic_prepare(dsd_opts* opts, dsd_state* state, uint8_t dt, int vox) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_UDP;
    opts->input_volume_multiplier = 1;
    opts->monitor_input_audio = 1;
    opts->audio_out = 1;
    opts->audio_out_type = 8;
    opts->audio_gainA = 0.0f;
    opts->rtl_squelch_mode = DSD_SQUELCH_MODE_LEVEL;
    opts->rtl_squelch_level = dB_to_pwr(-40.0);
    state->m17_rate = 8000;
    state->m17_can_en = -1;
    state->m17_vox = vox;
    state->m17_str_dt = dt;
    state->codec2_1600 = dt == 3U ? TEST_CODEC2_1600 : NULL;
}

/* One stream encoder session (encodeM17STR()) over @p script on @p opts / @p state as they stand. */
static void
mic_encode(dsd_opts* opts, dsd_state* state, const mic_segment* script, size_t len) {
    g_mic_script = script;
    g_mic_script_len = len;
    g_mic_reads = 0;
    g_mic_state = state;
    g_mic_phase = 0.0;
    g_mic_noise = 0x9E3779B97F4A7C15ULL;
    g_mic_iters = 0;
    DSD_MEMSET(g_mic_tx, 0xFF, sizeof g_mic_tx);
    DSD_MEMSET(g_mic_frame_at, 0, sizeof g_mic_frame_at);
    g_codec2_encode_count = 0;
    DSD_MEMSET(g_codec2_encode_peaks, 0, sizeof g_codec2_encode_peaks);
    DSD_MEMSET(g_codec2_encode_rms, 0, sizeof g_codec2_encode_rms);
    dsd_net_audio_input_hooks_set(&(dsd_net_audio_input_hooks){.udp_read_sample = fake_udp_mic_read});
    exitflag = 0;
    (void)encodeM17STR(opts, state);
    exitflag = 0;
    g_mic_frame_at[g_mic_iters] = g_codec2_encode_count;
    dsd_net_audio_input_hooks_set(NULL);
    g_mic_state = NULL;
}

/* Run the stream encoder once over @p script on a fresh setup (mic_prepare()). */
static void
mic_run(const mic_segment* script, size_t len, uint8_t dt, int vox) {
    static dsd_opts opts;
    static dsd_state state;
    mic_prepare(&opts, &state, dt, vox);
    mic_encode(&opts, &state, script, len);
    dsd_state_ext_free_all(&state);
}

/* The RMS level, in dB re full scale, of the codec2 frames read @p iter handed the encoder (all of them, or with
   @p first_only the first); -999 when it has none logged. */
static double
mic_iter_db(long iter, int first_only) {
    if (iter < 0 || iter >= g_mic_iters) {
        return -999.0;
    }
    const int from = g_mic_frame_at[iter];
    int to = first_only ? from + 1 : g_mic_frame_at[iter + 1];
    to = to < TEST_CODEC2_ENCODE_LOG ? to : TEST_CODEC2_ENCODE_LOG;
    double acc = 0.0;
    for (int f = from; f < to; f++) {
        acc += g_codec2_encode_rms[f] * g_codec2_encode_rms[f];
    }
    return to > from ? 10.0 * log10(acc / (double)(to - from)) - (20.0 * log10(32768.0)) : -999.0;
}

/* The peak of every codec2 frame from read @p first_iter to the end. */
static int
mic_peak_from(long first_iter) {
    int peak = 0;
    for (int f = g_mic_frame_at[first_iter]; f < g_codec2_encode_count && f < TEST_CODEC2_ENCODE_LOG; f++) {
        peak = g_codec2_encode_peaks[f] > peak ? g_codec2_encode_peaks[f] : peak;
    }
    return peak;
}

static int
expect_db_near(const char* label, double got, double want, double tol) {
    if (!(fabs(got - want) <= tol)) {
        DSD_FPRINTF(stderr, "%s: %.2f dB, want %.2f +- %.2f\n", label, got, want, tol);
        return 1;
    }
    return 0;
}

static const uint8_t k_mic_rates[2] = {2U, 3U};

/* Loud room noise while unkeyed does not move the gain: the next transmission starts at the level the last one ended
   on. (It used to pull the gain down more than 10 dB, which the next transmission then played at.) Held, the noise
   reaches codec2 limited to the AGC's target. */
static int
test_stream_encoder_mic_holds_while_unkeyed(void) {
    static const mic_segment script[] = {
        {50, 1, 3000.0, 0.0},
        {25, 0, 0.0, 12000.0},
        {10, 1, 3000.0, 0.0},
    };
    int err = 0;
    for (size_t r = 0U; r < 2U; r++) {
        mic_run(script, sizeof script / sizeof script[0], k_mic_rates[r], 0);
        const double before = mic_iter_db(49, 0);
        err |= expect_db_near("mic: a settled tone reaches codec2 at the AGC's target (-10 dBFS peak)", before,
                              20.0 * log10((double)DSD_VOICE_AGC_TARGET_PEAK / 32768.0) - 3.01, 0.1);
        err |= expect_db_near("mic: the first frame after unkeyed room noise", mic_iter_db(75, 1), before, 0.1);
        err |= expect_db_near("mic: the transmission after unkeyed room noise", mic_iter_db(80, 0), before, 0.1);
        err |= expect_int("mic: everything codec2 got stayed within the target",
                          mic_peak_from(0) <= (int)DSD_VOICE_AGC_TARGET_PEAK + 1, 1);
    }
    return err;
}

/* What a transmission taught stays for the next, however short it was. A 120 ms transmission 12 dB louder leaves the
   gain 12 dB lower (the next transmission's tone plays 12 dB under it, inside the envelope's hold); a long one whose
   gain rose over its last 250 ms leaves the risen gain (the next goes on where it ended: one more read's release, 0.8
   dB at 20 dB/s). A rollback at each key-up, as a receiver's squelch close does, would undo both. */
static int
test_stream_encoder_mic_keeps_gain_across_key_ups(void) {
    static const mic_segment short_tx[] = {
        {50, 1, 2000.0, 0.0}, {10, 0, 0.0, 0.0}, {3, 1, 8000.0, 0.0}, {10, 0, 0.0, 0.0}, {5, 1, 2000.0, 0.0},
    };
    static const mic_segment long_tx[] = {
        {50, 1, 2000.0, 0.0}, {5, 1, 8000.0, 0.0}, {20, 1, 2000.0, 0.0}, {10, 0, 0.0, 0.0}, {5, 1, 2000.0, 0.0},
    };
    int err = 0;
    for (size_t r = 0U; r < 2U; r++) {
        mic_run(short_tx, sizeof short_tx / sizeof short_tx[0], k_mic_rates[r], 0);
        const double taught = mic_iter_db(61, 0);
        err |= expect_db_near("mic: the next transmission after a 120 ms one", mic_iter_db(73, 0),
                              taught + (20.0 * log10(2000.0 / 8000.0)), 0.05);
        mic_run(long_tx, sizeof long_tx / sizeof long_tx[0], k_mic_rates[r], 0);
        const double ended = mic_iter_db(74, 0);
        const double next = mic_iter_db(85, 0);
        err |=
            expect_db_near("mic: the gain rose over the long transmission's end", ended - mic_iter_db(66, 0), 5.6, 0.2);
        err |= expect_db_near("mic: the next transmission after a long one", next - ended, 0.8, 0.1);
    }
    return err;
}

/* The gain starts over when the encoder starts: a second session on the same decoder state opens at the reference gain,
   as the first did, not at the gain the first learned (a quiet tone the first lifted 18 dB). */
static int
test_stream_encoder_mic_starts_over_at_encoder_start(void) {
    static const mic_segment script[] = {
        {50, 1, 1000.0, 0.0},
        {5, 0, 0.0, 0.0},
    };
    static dsd_opts opts;
    static dsd_state state;
    int err = 0;
    for (size_t r = 0U; r < 2U; r++) {
        mic_prepare(&opts, &state, k_mic_rates[r], 0);
        mic_encode(&opts, &state, script, sizeof script / sizeof script[0]);
        const double first_open = mic_iter_db(0, 1);
        const double first_learned = mic_iter_db(49, 0);
        mic_encode(&opts, &state, script, sizeof script / sizeof script[0]);
        err |= expect_db_near("mic: the first session learned a boost", first_learned - first_open, 18.0, 0.1);
        err |= expect_db_near("mic: a second session opens where the first did", mic_iter_db(0, 1), first_open, 0.01);
        dsd_state_ext_free_all(&state);
    }
    return err;
}

/* The VOX keys and unkeys on exactly the reads it always did (the sequence below is what the encoder did before the
   microphone learned its key state), and the read that keys it adapts the gain from its first frame: a tone 7 dB over
   the envelope the last transmission left plays at the target, not limited sample by sample (+1.8 dB). */
static int
test_stream_encoder_vox_keys_on_the_same_reads(void) {
    static const mic_segment script[] = {
        {30, -1, 1500.0, 0.0}, {8, -1, 0.0, 30.0},  {4, -1, 1500.0, 0.0}, {20, -1, 0.0, 30.0},
        {3, -1, 3000.0, 0.0},  {15, -1, 0.0, 30.0}, {2, -1, 3000.0, 0.0}, {14, -1, 0.0, 30.0},
    };
    /* m17encoder_tx as each read starts, the same at both rates (the VOX decides once per 40 ms read). */
    static const char want[] = "1111111111111111111111111111111111111111111111111111111"
                               "00000000"
                               "111111111111111111111111111111"
                               "000";
    int err = 0;
    for (size_t r = 0U; r < 2U; r++) {
        mic_run(script, sizeof script / sizeof script[0], k_mic_rates[r], 1);
        char got[MIC_MAX_ITERS + 1];
        for (long i = 0; i < g_mic_iters; i++) {
            got[i] = g_mic_tx[i] == 1 ? '1' : (g_mic_tx[i] == 0 ? '0' : '?');
        }
        got[g_mic_iters] = '\0';
        if (strcmp(got, want) != 0) {
            DSD_FPRINTF(stderr, "vox keying (%s): got  %s\n                    want %s\n", r == 0U ? "3200" : "1600",
                        got, want);
            err = 1;
        }
        err |= expect_db_near("vox: the read that keys plays its tone at the target", mic_iter_db(62, 1),
                              20.0 * log10((double)DSD_VOICE_AGC_TARGET_PEAK / 32768.0) - 3.01, 0.1);
    }
    return err;
}

static int
test_stream_voice_audio_follows_output_format(void) {
    static const uint8_t dts[] = {2U, 3U};
    int err = 0;
    for (size_t d = 0U; d < sizeof(dts) / sizeof(dts[0]); d++) {
        for (int floating = 0; floating <= 1; floating++) {
            for (int channels = 1; channels <= 2; channels++) {
                for (int gain = 1; gain <= 3; gain += 2) {
                    err |= run_voice_format_case(dts[d], floating, channels, 8, (float)gain, NULL, NULL);
#ifdef DSD_NEO_TEST_AUDIO_WRAP
                    err |= run_voice_format_case(dts[d], floating, channels, 0, (float)gain, NULL, NULL);
#endif
                }
            }
        }
        /* -g 1 passes the decoded samples through; -g 3 triples them, in both channels. */
        int16_t unity[320];
        int16_t tripled[320];
        size_t unity_count = 0U;
        size_t tripled_count = 0U;
        err |= run_voice_format_case(dts[d], 0, 2, 8, 1.0f, unity, &unity_count);
        err |= run_voice_format_case(dts[d], 0, 2, 8, 3.0f, tripled, &tripled_count);
        err |= expect_int("m17 voice gain block sizes", (int)unity_count == 320 && tripled_count == unity_count, 1);
        for (size_t i = 0U; i < unity_count; i++) {
            const int want = dts[d] == 3U ? 1001 + 160 + (int)(i / 2U) : 1002 + (int)(i / 2U);
            err |= expect_int("m17 voice at -g 1", unity[i], want);
            err |= expect_int("m17 voice at -g 3", tripled[i], want * 3);
        }
    }
    return err;
}
#endif

#ifdef USE_CODEC2
/* Issue #574: Codec2 voice stamps only what an output receives. The null output (-o null, which keeps output type 9 once
   unmuted) and a local stream that is not open receive nothing from either payload (the 3200 pair, the 1600 single), so
   a frame that would play stamps nothing. */
static int
test_stream_voice_audio_stamps_nothing_no_output_takes(void) {
    static const struct {
        const char* tag;
        uint8_t dt;
    } formats[] = {
        {"3200", 2U},
        {"1600", 3U},
    };

    static const struct {
        const char* tag;
        int out_type;
    } outputs[] = {
        {"null output", 9},
        {"local stream not open", 0},
    };

    static const uint8_t payload_bytes[16] = {0x60U, 0x61U, 0x62U, 0x63U, 0x64U, 0x65U, 0x66U, 0x67U,
                                              0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U};
    uint8_t payload_bits[128];
    uint8_t processed_bits[128];
    bytes_to_bits(payload_bytes, payload_bits, sizeof(payload_bytes));

    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    int err = 0;
    dsd_audio_activity_arm();
    for (size_t f = 0U; f < sizeof(formats) / sizeof(formats[0]); f++) {
        for (size_t o = 0U; o < sizeof(outputs) / sizeof(outputs[0]); o++) {
            DSD_MEMSET(opts, 0, sizeof(*opts));
            DSD_MEMSET(state, 0, sizeof(*state));
            reset_audio_fakes();
            install_fake_udp_audio();
            prepare_voice_output(opts, state);
            g_codec2_decode_samples = formats[f].dt == 3U ? 320U : 160U;
            opts->slot1_on = 1;
            opts->audio_out = 1;
            opts->audio_out_type = outputs[o].out_type;
            opts->audio_out_stream = NULL;
            opts->audio_out_fd = -1;
            state->m17_str_dt = formats[f].dt;
            state->m17_can_en = -1;
            m17_confirm_note_evidence(NULL, state, M17_EVIDENCE_STRONG);
            dsd_audio_activity_reset();

            /* With no stream, the platform's own write refuses the block, as it does in a session. */
            (void)m17_dispatch_stream_payload(opts, state, payload_bits, M17_REF_STREAM_FN, processed_bits);

            uint64_t stamp = 0U;
            dsd_audio_activity_read(&stamp, NULL);
            char label[128];
            DSD_SNPRINTF(label, sizeof(label), "m17 %s %s: udp blocks", formats[f].tag, outputs[o].tag);
            err |= expect_int(label, g_udp_audio_calls, 0);
            DSD_SNPRINTF(label, sizeof(label), "m17 %s %s: stamp", formats[f].tag, outputs[o].tag);
            err |= expect_int(label, stamp != 0U, 0);
            reset_audio_fakes();
            dsd_state_ext_free_all(state);
        }
    }
    dsd_audio_activity_reset();
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
    dsd_rtl_stream_io_hooks_set(&(dsd_rtl_stream_io_hooks){
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
    dsd_rtl_stream_io_hooks_set(NULL);
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
    dsd_rtl_stream_io_hooks_set(&(dsd_rtl_stream_io_hooks){
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
    dsd_rtl_stream_io_hooks_set(NULL);
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

/* ---- Issue #890: LSF evidence, checked fields and the stream signature ---- */

/* The LSF bits (CRC attached) of a stream from M17_REF_LSF_SRC_CSD to M17_REF_LSF_DST_CSD with @p type_word. */
static void
build_checked_lsf(uint8_t lsf_bits[TEST_M17_LSF_BITS], uint8_t lsf_packed[M17_LSF_BYTES], uint16_t type_word,
                  uint16_t* crc) {
    const unsigned long long dst = m17_encode_b40_callsign(0ULL, M17_REF_LSF_DST_CSD);
    const unsigned long long src = m17_encode_b40_callsign(0ULL, M17_REF_LSF_SRC_CSD);
    build_lsf_bits(lsf_bits, dst, src, type_word, NULL);
    const uint16_t c = m17_attach_lsf_crc(lsf_bits, lsf_packed);
    if (crc != NULL) {
        *crc = c;
    }
}

/* One stream frame's LICH chunk (@p counter of @p lsf_bits) through the frame's evidence accounting. */
static int
feed_lich_frame(dsd_opts* opts, dsd_state* state, const uint8_t lsf_bits[TEST_M17_LSF_BITS], uint8_t counter) {
    uint8_t encoded[M17_LICH_BITS];
    build_encoded_lich_chunk(lsf_bits, counter, encoded);
    m17_confirm_begin_frame(state);
    const int rc = m17_process_lich(state, opts, encoded);
    m17_confirm_end_frame(state);
    return rc;
}

/* An LSF rebuilt from six LICH chunks under one binding with its CRC passing is checked content, as the LSF frame's own
   CRC is: it proves the transmission and tells a scan quiet check so (PROVEN). One -F decodes despite a failed CRC
   proves nothing. */
static int
test_rebuilt_lsf_is_strong_evidence(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t lsf_packed[M17_LSF_BYTES];
    int err = 0;

    dsd_state_ext_free_all(state);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->aggressive_framesync = 1;
    build_checked_lsf(lsf_bits, lsf_packed, m17_compose_frame_info(1U, 2U, 0U, 0U, 6U, 0U, 0U), NULL);
    const uint32_t seq0 = state->scan_y_content_seq;
    for (uint8_t chunk = 0U; chunk < (M17_LICH_CHUNKS - 1U); chunk++) {
        err |= expect_int("rebuild chunk", feed_lich_frame(opts, state, lsf_bits, chunk), 0);
    }
    err |= expect_int("partial rebuild proves nothing", (int)(state->scan_y_content_seq - seq0), 0);
    err |= expect_int("partial rebuild confirms nothing", m17_confirm_is_confirmed(state), 0);
    err |= expect_int("final rebuild chunk", feed_lich_frame(opts, state, lsf_bits, M17_LICH_CHUNKS - 1U), 0);
    err |= expect_int("checked rebuild is PROVEN content", (int)(state->scan_y_content_seq - seq0), 1);
    err |= expect_int("checked rebuild confirms the transmission", m17_confirm_is_confirmed(state), 1);
    dsd_call_snapshot call;
    err |= expect_int("checked rebuild publishes its call", get_m17_call(state, &call), 1);

    /* -F: the same LSF with its CRC broken decodes, and proves nothing. */
    dsd_state_ext_free_all(state);
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->aggressive_framesync = 0;
    lsf_bits[M17_LSF_LSD_BITS + 5U] ^= 1U;
    const uint32_t seq1 = state->scan_y_content_seq;
    for (uint8_t chunk = 0U; chunk < M17_LICH_CHUNKS; chunk++) {
        err |= expect_int("relaxed rebuild chunk", feed_lich_frame(opts, state, lsf_bits, chunk), 0);
    }
    err |= expect_int("relaxed rebuild is no content proof", (int)(state->scan_y_content_seq - seq1), 0);
    err |= expect_int("relaxed rebuild confirms nothing", m17_confirm_is_confirmed(state), 0);
    err |= expect_u8("relaxed rebuild still decodes for -F", state->m17_can, 6U);
    dsd_state_ext_free_all(state);
    return err;
}

/* -F decodes an LSF whose CRC failed, and while no checked LSF bound the transmission its values are all there is. Once
   one did, a relaxed one is shown and nothing more: the checked CAN (0 included), encryption, stream type and IV stay
   the transmission's. */
static int
test_relaxed_lsf_keeps_checked_fields(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t checked_bits[TEST_M17_LSF_BITS];
    uint8_t checked_packed[M17_LSF_BYTES];
    uint8_t relaxed_bits[TEST_M17_LSF_BITS];
    uint8_t relaxed_packed[M17_LSF_BYTES];
    uint16_t checked_crc = 0U;
    int err = 0;

    dsd_state_ext_free_all(state);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->aggressive_framesync = 0;
    build_checked_lsf(checked_bits, checked_packed, m17_compose_frame_info(1U, 2U, 0U, 0U, 0U, 0U, 0U), &checked_crc);
    /* The relaxed one: AES, 1600 voice, CAN 9, with an IV, its CRC broken. */
    const unsigned long long dst = m17_encode_b40_callsign(0ULL, M17_REF_LSF_DST_CSD);
    const unsigned long long src = m17_encode_b40_callsign(0ULL, M17_REF_LSF_SRC_CSD);
    build_lsf_bits(relaxed_bits, dst, src, m17_compose_frame_info(1U, 3U, 2U, 0U, 9U, 0U, 0U), M17_REF_AES_NONCE);
    (void)m17_attach_lsf_crc(relaxed_bits, relaxed_packed);
    relaxed_bits[M17_LSF_LSD_BITS + 2U] ^= 1U;

    DSD_MEMCPY(state->m17_lsf, checked_bits, sizeof(checked_bits));
    err |= expect_int("checked LSF frame passes", m17_finalize_lsf_crc(opts, state, checked_packed, checked_crc), 0);
    err |= expect_u8("checked CAN 0", state->m17_can, 0U);
    for (uint8_t chunk = 0U; chunk < M17_LICH_CHUNKS; chunk++) {
        (void)feed_lich_frame(opts, state, relaxed_bits, chunk);
    }
    err |= expect_u8("relaxed rebuild keeps the checked CAN 0", state->m17_can, 0U);
    err |= expect_u8("relaxed rebuild keeps the checked encryption", state->m17_enc, 0U);
    err |= expect_u8("relaxed rebuild keeps the checked stream type", state->m17_str_dt, 2U);
    uint8_t zero_meta[sizeof(state->m17_meta)];
    DSD_MEMSET(zero_meta, 0, sizeof(zero_meta));
    err |= expect_bytes("relaxed rebuild keeps the checked IV", state->m17_meta, zero_meta, sizeof(zero_meta));

    /* A checked rebuild is checked: it binds its values. */
    uint8_t rebound_bits[TEST_M17_LSF_BITS];
    uint8_t rebound_packed[M17_LSF_BYTES];
    build_checked_lsf(rebound_bits, rebound_packed, m17_compose_frame_info(1U, 2U, 0U, 0U, 4U, 0U, 0U), NULL);
    for (uint8_t chunk = 0U; chunk < M17_LICH_CHUNKS; chunk++) {
        (void)feed_lich_frame(opts, state, rebound_bits, chunk);
    }
    err |= expect_u8("checked rebuild binds its CAN", state->m17_can, 4U);

    /* At a transmission boundary the binding goes, and -F has the relaxed values again. */
    m17_confirm_reset(state);
    for (uint8_t chunk = 0U; chunk < M17_LICH_CHUNKS; chunk++) {
        (void)feed_lich_frame(opts, state, relaxed_bits, chunk);
    }
    err |= expect_u8("relaxed rebuild after a boundary decodes its CAN", state->m17_can, 9U);
    err |= expect_u8("relaxed rebuild after a boundary decodes its encryption", state->m17_enc, 2U);
    dsd_state_ext_free_all(state);
    return err;
}

/* The payloads of a signed stream of @p frames voice frames whose digest (M17 spec 2.0.11, Message Digest Algorithm for
   Voice Streams) comes to M17_REF_SIGNATURE_DIGEST, which M17_REF_SIGNATURE_BYTES signs: the last frame's payload is
   solved for it. */
static void
build_signed_payloads(uint8_t payloads[][M17_SIGNATURE_DIGEST_BYTES], size_t frames) {
    uint8_t digest[M17_SIGNATURE_DIGEST_BYTES];
    DSD_MEMSET(digest, 0, sizeof(digest));
    for (size_t f = 0U; f + 1U < frames; f++) {
        for (size_t i = 0U; i < M17_SIGNATURE_DIGEST_BYTES; i++) {
            payloads[f][i] = (uint8_t)((f * 37U) + (i * 11U) + 5U);
        }
        m17_signature_digest_update(digest, payloads[f]);
    }
    /* rol(digest ^ p, 8) = target: p = ror(target, 8) ^ digest. */
    for (size_t i = 0U; i < M17_SIGNATURE_DIGEST_BYTES; i++) {
        const size_t from = (i + M17_SIGNATURE_DIGEST_BYTES - 1U) % M17_SIGNATURE_DIGEST_BYTES;
        payloads[frames - 1U][i] = (uint8_t)(M17_REF_SIGNATURE_DIGEST[from] ^ digest[i]);
    }
}

enum { TEST_SIGNED_VOICE_FRAMES = 16 };

static const uint16_t k_test_signature_fns[4] = {M17_STREAM_SIGNATURE_FN0, M17_STREAM_SIGNATURE_FN1,
                                                 M17_STREAM_SIGNATURE_FN2, M17_STREAM_SIGNATURE_FN3};

/* A signed RF stream: its LSF frame, 16 voice frames and the four signature frames, each stream frame with its LICH
   chunk. The LSF is rebuilt from the LICH three times on the way, the last between the first and second signature
   frames; none of those rebuilds is a transmission boundary, so the digest and the signature collected so far survive
   them and the signature verifies against the station's public key. */
static int
test_signed_rf_stream_verifies_across_lich_rebuilds(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t lsf_packed[M17_LSF_BYTES];
    uint8_t payloads[TEST_SIGNED_VOICE_FRAMES][M17_SIGNATURE_DIGEST_BYTES];
    uint8_t payload_bits[M17_STREAM_PAYLOAD_BITS];
    uint8_t processed_bits[M17_STREAM_PAYLOAD_BITS];
    uint16_t crc = 0U;
    int err = 0;

    reset_audio_fakes();
    dsd_state_ext_free_all(state);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->aggressive_framesync = 1;
    state->m17_can_en = -1;
    DSD_MEMCPY(state->m17_signature_public_key, M17_REF_SIGNATURE_PUBLIC_KEY, sizeof(M17_REF_SIGNATURE_PUBLIC_KEY));
    state->m17_signature_public_key_loaded = 1U;
    build_checked_lsf(lsf_bits, lsf_packed, m17_compose_frame_info(1U, 2U, 0U, 0U, 3U, 1U, 0U), &crc);
    build_signed_payloads(payloads, TEST_SIGNED_VOICE_FRAMES);

    DSD_MEMCPY(state->m17_lsf, lsf_bits, sizeof(lsf_bits));
    err |= expect_int("signed LSF frame passes", m17_finalize_lsf_crc(opts, state, lsf_packed, crc), 0);
    err |= expect_u8("signed LSF advertises the signature", state->m17_signature_advertised, 1U);

    int frame = 0;
    for (int f = 0; f < TEST_SIGNED_VOICE_FRAMES; f++, frame++) {
        (void)feed_lich_frame(opts, state, lsf_bits, (uint8_t)(frame % (int)M17_LICH_CHUNKS));
        bytes_to_bits(payloads[f], payload_bits, M17_SIGNATURE_DIGEST_BYTES);
        (void)m17_dispatch_stream_payload(opts, state, payload_bits, (uint16_t)f, processed_bits);
    }
    err |= expect_bytes("digest of the voice frames", state->m17_signature_digest, M17_REF_SIGNATURE_DIGEST,
                        sizeof(M17_REF_SIGNATURE_DIGEST));
    for (size_t i = 0U; i < 4U; i++, frame++) {
        (void)feed_lich_frame(opts, state, lsf_bits, (uint8_t)(frame % (int)M17_LICH_CHUNKS));
        expect_signature_slice(payload_bits, i * 16U);
        err |=
            expect_int("signature frame consumed",
                       m17_dispatch_stream_payload(opts, state, payload_bits, k_test_signature_fns[i], processed_bits),
                       M17_STREAM_SIGNATURE_CONSUMED);
    }
    err |= expect_u8("signature complete", state->m17_signature_complete, 1U);
    err |= expect_u8("signature in sequence", state->m17_signature_bad_sequence, 0U);
    err |= expect_u8("signed RF stream verifies", state->m17_signature_verification_status, 1U);
    dsd_state_ext_free_all(state);
    return err;
}

static void
send_ip_stream_frame(dsd_opts* opts, dsd_state* state, const uint8_t lsf_bits[TEST_M17_LSF_BITS], uint16_t sid,
                     uint16_t fn, uint8_t eot, const uint8_t payload[M17_SIGNATURE_DIGEST_BYTES], int corrupt) {
    uint8_t ip_frame[54];
    uint8_t payload_bits[M17_STREAM_PAYLOAD_BITS];
    bytes_to_bits(payload, payload_bits, M17_SIGNATURE_DIGEST_BYTES);
    build_ip_stream_frame(ip_frame, lsf_bits, sid, fn, eot, payload_bits);
    if (corrupt) {
        ip_frame[40] ^= 0x5AU; /* inside the payload; the CRC no longer covers it */
    }
    m17_ip_dispatch_frame(opts, state, ip_frame, (int)sizeof(ip_frame));
}

static void ip_test_state(dsd_opts* opts, dsd_state* state);

/* A signed network stream (issue #890: M17 signatures are not verified on network streams): its signature frames are
   consumed, never played or collected, there is no digest, and the stream is reported not verified over the network
   from its first frame's link setup.
   A copy of a frame that arrives again (two network paths), or one whose CRC failed, plays nothing. After the EOT, a
   copy of the last frame opens no call row and plays nothing; frame 0 of the same stream ID, or another stream ID,
   starts a stream of its own. */
static int
test_signed_ip_stream_is_not_verified(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t lsf_packed[M17_LSF_BYTES];
    uint8_t payloads[TEST_SIGNED_VOICE_FRAMES][M17_SIGNATURE_DIGEST_BYTES];
    int err = 0;

    ip_test_state(opts, state);
    build_checked_lsf(lsf_bits, lsf_packed, m17_compose_frame_info(1U, 2U, 0U, 0U, 3U, 1U, 0U), NULL);
    build_signed_payloads(payloads, TEST_SIGNED_VOICE_FRAMES);

    const uint16_t sid = 0x1234U;
    for (int f = 0; f < TEST_SIGNED_VOICE_FRAMES; f++) {
        send_ip_stream_frame(opts, state, lsf_bits, sid, (uint16_t)f, 0U, payloads[f], 0);
        if (f == 7 || f == 8) {
            const int decodes = g_codec2_decode_calls;
            send_ip_stream_frame(opts, state, lsf_bits, sid, (uint16_t)f, 0U, payloads[f], f == 7);
            err |= expect_int("a copy plays nothing", g_codec2_decode_calls - decodes, 0);
        }
    }
    uint8_t zero[M17_SIGNATURE_DIGEST_BYTES];
    DSD_MEMSET(zero, 0, sizeof(zero));
    err |= expect_bytes("a network stream has no digest", state->m17_signature_digest, zero, sizeof(zero));
    for (size_t i = 0U; i < 4U; i++) {
        const uint16_t fn = k_test_signature_fns[i];
        const int decodes = g_codec2_decode_calls;
        send_ip_stream_frame(opts, state, lsf_bits, sid, (uint16_t)(fn & M17_STREAM_FRAME_COUNTER_MAX),
                             (uint8_t)((fn & M17_STREAM_FRAME_END_MASK) != 0U), M17_REF_SIGNATURE_BYTES + (i * 16U), 0);
        err |= expect_int("a signature frame plays nothing", g_codec2_decode_calls - decodes, 0);
        if (i == 2U) {
            /* Before its last frame: the verdict, and no chunk; its end drops everything held from it. */
            err |= expect_u8("no signature chunk is collected", state->m17_signature_received_mask, 0U);
            err |= expect_u8("the network stream is not verified", state->m17_signature_verification_status, 6U);
        }
    }
    err |= expect_u8("its end drops the verdict with the rest of it", state->m17_signature_verification_status, 0U);

    dsd_call_snapshot ended_call;
    err |= expect_int("signed IP stream had a call", get_m17_call(state, &ended_call), 1);
    err |= expect_int("its EOT ended the call", ended_call.phase == DSD_CALL_PHASE_ACTIVE, 0);
    state->m17_confirmed = 1U;
    const int decodes_before_copy = g_codec2_decode_calls;
    send_ip_stream_frame(opts, state, lsf_bits, sid,
                         (uint16_t)(M17_STREAM_SIGNATURE_FN3 & M17_STREAM_FRAME_COUNTER_MAX), 1U,
                         M17_REF_SIGNATURE_BYTES + 48U, 0);
    dsd_call_snapshot copy_call;
    err |= expect_int("a repeated last frame keeps the call record", get_m17_call(state, &copy_call), 1);
    err |= expect_int("a repeated last frame opens no call row", copy_call.phase == DSD_CALL_PHASE_ACTIVE, 0);
    err |= expect_u64("a repeated last frame starts no epoch", copy_call.epoch, ended_call.epoch);
    err |= expect_int("a repeated last frame plays nothing", g_codec2_decode_calls - decodes_before_copy, 0);
    state->m17_confirmed = 0U;

    send_ip_stream_frame(opts, state, lsf_bits, sid, 0U, 0U, payloads[0], 0);
    /* A signed stream is reported not verified over the network from its link setup, its frame 0 here. */
    err |= expect_u8("a restarted signed stream ID is reported from its own link setup",
                     state->m17_signature_verification_status, 6U);
    err |= expect_int("a restarted stream ID starts at its frame 0", (int)state->m17_ip_stream_last_fn, 0);
    err |= expect_int("a restarted stream ID is open", (int)state->m17_ip_stream, DSD_M17_IP_STREAM_OPEN);
    send_ip_stream_frame(opts, state, lsf_bits, (uint16_t)(sid + 1U), 0U, 0U, payloads[0], 0);
    err |= expect_int("another stream ID opens", (int)state->m17_ip_stream_sid, (int)(sid + 1U));
    dsd_state_ext_free_all(state);
    return err;
}

/* An IP control frame (@p magic, "EOTX" or "DISC") naming station @p callsign, as an M17 reflector sends it. */
static void
build_ip_control(uint8_t out[10], const char* magic, const char* callsign) {
    DSD_MEMCPY(out, magic, 4U);
    const unsigned long long station = m17_encode_b40_callsign(0ULL, callsign);
    for (unsigned i = 0U; i < 6U; i++) {
        out[4U + i] = (uint8_t)(station >> (40U - (8U * i)));
    }
}

/* An M17 end (an IP stream's EOT, EOTX) ends the slot's call only when it is an M17 one: another protocol's call still
   open on the slot is no transmission of it (issue #890). */
static int
test_m17_end_ends_only_an_m17_call(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    int err = 0;
    static const int protocols[] = {DSD_SYNC_DMR_BS_VOICE_POS, DSD_SYNC_M17_STR_POS};
    for (size_t p = 0U; p < sizeof protocols / sizeof protocols[0]; p++) {
        const int is_m17 = DSD_SYNC_IS_M17(protocols[p]);
        for (int end_kind = 0; end_kind < 2; end_kind++) {
            dsd_state_ext_free_all(state);
            DSD_MEMSET(opts, 0, sizeof(*opts));
            DSD_MEMSET(state, 0, sizeof(*state));
            opts->aggressive_framesync = 1;
            const dsd_call_observation open = {
                .protocol = protocols[p],
                .slot = 0U,
                .kind = DSD_CALL_KIND_VOICE,
                .ota_target_id = 1201U,
                .ota_source_id = 4201U,
            };
            err |= expect_int("call opens", dsd_call_state_observe(state, &open, DSD_CALL_BOUNDARY_BEGIN), 1);
            /* A network stream of data type 0 observes no call, so only its end can touch the open one: its EOTX
               (naming its station) or its EOT frame. */
            uint8_t lsf_bits[TEST_M17_LSF_BITS];
            uint8_t lsf_packed[M17_LSF_BYTES];
            uint8_t payload[M17_SIGNATURE_DIGEST_BYTES];
            DSD_MEMSET(payload, 0, sizeof(payload));
            build_checked_lsf(lsf_bits, lsf_packed, m17_compose_frame_info(1U, 0U, 0U, 0U, 1U, 0U, 0U), NULL);
            if (end_kind == 0) {
                send_ip_stream_frame(opts, state, lsf_bits, 0x0042U, 0U, 0U, payload, 0);
                uint8_t eotx[10];
                build_ip_control(eotx, "EOTX", M17_REF_LSF_SRC_CSD);
                m17_ip_dispatch_frame(opts, state, eotx, sizeof(eotx));
            } else {
                send_ip_stream_frame(opts, state, lsf_bits, 0x0042U, 3U, 1U, payload, 0);
            }
            dsd_call_snapshot call;
            err |= expect_int("call still known", get_m17_call(state, &call), 1);
            err |= expect_int(is_m17 ? "an M17 end ends the M17 call" : "an M17 end leaves another protocol's call",
                              call.phase == DSD_CALL_PHASE_ACTIVE, is_m17 ? 0 : 1);
        }
    }
    dsd_state_ext_free_all(state);
    return err;
}

/* After an IP stream's EOT, a copy of its last frame arriving again is that ended stream's: it opens no call row and
   plays nothing, even on a session whose transmission evidence is still standing. The same stream ID starting again
   from frame 0 is another transmission and opens its call. */
static int
test_ip_late_copy_after_eot_is_inert(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t lsf_packed[M17_LSF_BYTES];
    uint8_t payload[M17_SIGNATURE_DIGEST_BYTES];
    int err = 0;

    reset_audio_fakes();
    dsd_state_ext_free_all(state);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->aggressive_framesync = 1;
    state->m17_can_en = -1;
    build_checked_lsf(lsf_bits, lsf_packed, m17_compose_frame_info(1U, 2U, 0U, 0U, 3U, 0U, 0U), NULL);
    DSD_MEMSET(payload, 0x3C, sizeof(payload));

    const uint16_t sid = 0x2345U;
    for (uint16_t fn = 0U; fn < 4U; fn++) {
        send_ip_stream_frame(opts, state, lsf_bits, sid, fn, (uint8_t)(fn == 3U), payload, 0);
    }
    dsd_call_snapshot call;
    err |= expect_int("unsigned IP stream had a call", get_m17_call(state, &call), 1);
    err |= expect_int("its EOT ended the call", call.phase == DSD_CALL_PHASE_ACTIVE, 0);
    const uint64_t ended_epoch = call.epoch;

    state->m17_confirmed = 1U; /* evidence the IP EOT does not clear */
    const int decodes = g_codec2_decode_calls;
    send_ip_stream_frame(opts, state, lsf_bits, sid, 3U, 1U, payload, 0);
    err |= expect_int("a late copy keeps the call record", get_m17_call(state, &call), 1);
    err |= expect_int("a late copy opens no call row", call.phase == DSD_CALL_PHASE_ACTIVE, 0);
    err |= expect_u64("a late copy starts no epoch", call.epoch, ended_epoch);
    err |= expect_int("a late copy plays nothing", g_codec2_decode_calls - decodes, 0);

    send_ip_stream_frame(opts, state, lsf_bits, sid, 0U, 0U, payload, 0);
    err |= expect_int("a restart has a call", get_m17_call(state, &call), 1);
    err |= expect_int("a restart opens its call row", call.phase == DSD_CALL_PHASE_ACTIVE, 1);
    dsd_state_ext_free_all(state);
    return err;
}

/* Gate round 1 (issue #890): the network stream's lifecycle, its digest and the checked IP frames. */

/* A fresh state for an IP stream test, with the public key loaded and the CAN filter open. */
static void
ip_test_state(dsd_opts* opts, dsd_state* state) {
    reset_audio_fakes();
    dsd_state_ext_free_all(state);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->aggressive_framesync = 1;
    state->m17_can_en = -1;
    DSD_MEMCPY(state->m17_signature_public_key, M17_REF_SIGNATURE_PUBLIC_KEY, sizeof(M17_REF_SIGNATURE_PUBLIC_KEY));
    state->m17_signature_public_key_loaded = 1U;
}

/* After an IP stream ends on frame 0 (a one-frame call), the same stream ID starting again from frame 0 is another
   transmission, and the frames after it are that transmission's: it opens its call and keeps it open. */
static int
test_ip_stream_id_reused_after_one_frame_call(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t lsf_packed[M17_LSF_BYTES];
    uint8_t payload[M17_SIGNATURE_DIGEST_BYTES];
    int err = 0;

    ip_test_state(opts, state);
    build_checked_lsf(lsf_bits, lsf_packed, m17_compose_frame_info(1U, 2U, 0U, 0U, 3U, 0U, 0U), NULL);
    DSD_MEMSET(payload, 0x5A, sizeof(payload));
    const uint16_t sid = 0x0777U;
    send_ip_stream_frame(opts, state, lsf_bits, sid, 0U, 1U, payload, 0);
    dsd_call_snapshot call;
    err |= expect_int("one-frame call record", get_m17_call(state, &call), 1);
    err |= expect_int("one-frame call ended", call.phase == DSD_CALL_PHASE_ACTIVE, 0);
    const uint64_t first_epoch = call.epoch;
    for (uint16_t fn = 0U; fn < 3U; fn++) {
        send_ip_stream_frame(opts, state, lsf_bits, sid, fn, 0U, payload, 0);
    }
    err |= expect_int("reused stream ID call record", get_m17_call(state, &call), 1);
    err |= expect_int("reused stream ID opens its call", call.phase == DSD_CALL_PHASE_ACTIVE, 1);
    err |= expect_int("reused stream ID is another epoch", call.epoch != first_epoch, 1);
    dsd_state_ext_free_all(state);
    return err;
}

/* A receiver that joins a signed IP stream during its signature frames counts no voice frame; an exact copy of the
   stream's last frame (the EOT) arriving again is still that ended stream's: no new call epoch, and the signature
   collected so far is not replaced by that one chunk. */
static int
test_ip_late_copy_after_joining_on_signature_frames(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t lsf_packed[M17_LSF_BYTES];
    int err = 0;

    ip_test_state(opts, state);
    build_checked_lsf(lsf_bits, lsf_packed, m17_compose_frame_info(1U, 2U, 0U, 0U, 3U, 1U, 0U), NULL);
    const uint16_t sid = 0x0999U;
    for (size_t i = 0U; i < 4U; i++) {
        const uint16_t fn = k_test_signature_fns[i];
        send_ip_stream_frame(opts, state, lsf_bits, sid, (uint16_t)(fn & M17_STREAM_FRAME_COUNTER_MAX),
                             (uint8_t)((fn & M17_STREAM_FRAME_END_MASK) != 0U), M17_REF_SIGNATURE_BYTES + (i * 16U), 0);
        if (i == 2U) {
            err |= expect_u8("joined on the signature: not verified over the network",
                             state->m17_signature_verification_status, 6U);
        }
    }
    dsd_call_snapshot call;
    err |= expect_int("joined stream call record", get_m17_call(state, &call), 1);
    const uint64_t epoch = call.epoch;
    send_ip_stream_frame(opts, state, lsf_bits, sid,
                         (uint16_t)(M17_STREAM_SIGNATURE_FN3 & M17_STREAM_FRAME_COUNTER_MAX), 1U,
                         M17_REF_SIGNATURE_BYTES + 48U, 0);
    err |= expect_int("copy after the join: call record", get_m17_call(state, &call), 1);
    err |= expect_u64("copy after the join starts no epoch", call.epoch, epoch);
    err |= expect_int("copy after the join opens no call row", call.phase == DSD_CALL_PHASE_ACTIVE, 0);
    err |= expect_u8("copy after the join collects nothing", state->m17_signature_received_mask, 0U);
    dsd_state_ext_free_all(state);
    return err;
}

/* A checked IP voice frame is checked content of its own (its CRC): it plays without RF confirmation. An IP frame
   whose CRC failed is nothing: it plays nothing, marks no media, and after the stream's end (EOTX) brings back no
   carrier, sync type or staged LSF. */
static int
test_ip_checked_and_crc_failed_voice(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t lsf_packed[M17_LSF_BYTES];
    uint8_t payload[M17_SIGNATURE_DIGEST_BYTES];
    int err = 0;

    ip_test_state(opts, state);
    build_checked_lsf(lsf_bits, lsf_packed, m17_compose_frame_info(1U, 2U, 0U, 0U, 3U, 0U, 0U), NULL);
    DSD_MEMSET(payload, 0x66, sizeof(payload));
    const uint16_t sid = 0x0ABCU;
    int decodes = g_codec2_decode_calls;
    send_ip_stream_frame(opts, state, lsf_bits, sid, 0U, 0U, payload, 0);
#ifdef USE_CODEC2
    err |= expect_int("a checked IP voice frame plays", g_codec2_decode_calls - decodes, 2);
#endif
    dsd_call_snapshot call;
    err |= expect_int("checked IP call record", get_m17_call(state, &call), 1);
    const double media_before = call.media_updated_m;

    decodes = g_codec2_decode_calls;
    send_ip_stream_frame(opts, state, lsf_bits, sid, 1U, 0U, payload, 1);
    err |= expect_int("a CRC-failed IP voice frame plays nothing", g_codec2_decode_calls - decodes, 0);
    err |= expect_int("CRC-failed frame call record", get_m17_call(state, &call), 1);
    err |= expect_int("a CRC-failed IP voice frame marks no media", call.media_updated_m > media_before, 0);

    uint8_t eotx[10];
    build_ip_control(eotx, "EOTX", M17_REF_LSF_SRC_CSD);
    m17_ip_dispatch_frame(opts, state, eotx, sizeof(eotx));
    err |= expect_int("its station's EOTX drops the carrier", state->carrier, 0);
    DSD_MEMSET(state->m17_lsf, 0x77, sizeof(state->m17_lsf));
    decodes = g_codec2_decode_calls;
    send_ip_stream_frame(opts, state, lsf_bits, sid, 2U, 0U, payload, 1);
    err |= expect_int("a damaged late datagram brings no carrier back", state->carrier, 0);
    err |= expect_int("a damaged late datagram sets no sync type", state->synctype, DSD_SYNC_NONE);
    err |= expect_u8("a damaged late datagram stages no LSF", state->m17_lsf[0], 0x77U);
    err |= expect_int("a damaged late datagram plays nothing", g_codec2_decode_calls - decodes, 0);
    dsd_state_ext_free_all(state);
    return err;
}

/* A signed LSF frame whose CRC passed, then a repeat of it whose CRC failed: the repeat resets the digest (no stream
   frame has been sent yet) but does not take back the checked signature advertisement, so the signature frames are
   still collected, never voice. */
static int
test_bad_repeated_lsf_keeps_signature_advertised(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t lsf_packed[M17_LSF_BYTES];
    uint8_t payload_bits[M17_STREAM_PAYLOAD_BITS];
    uint8_t processed_bits[M17_STREAM_PAYLOAD_BITS];
    uint16_t crc = 0U;
    int err = 0;

    ip_test_state(opts, state);
    build_checked_lsf(lsf_bits, lsf_packed, m17_compose_frame_info(1U, 2U, 0U, 0U, 3U, 1U, 0U), &crc);
    DSD_MEMCPY(state->m17_lsf, lsf_bits, sizeof(lsf_bits));
    err |= expect_int("signed LSF frame passes", m17_finalize_lsf_crc(opts, state, lsf_packed, crc), 0);
    DSD_MEMCPY(state->m17_lsf, lsf_bits, sizeof(lsf_bits));
    err |= expect_int("its bad repeat fails", m17_finalize_lsf_crc(opts, state, lsf_packed, crc ^ 1U), 1);
    err |= expect_u8("the bad repeat keeps the checked advertisement", state->m17_signature_advertised, 1U);
    const int decodes = g_codec2_decode_calls;
    expect_signature_slice(payload_bits, 0U);
    err |= expect_int("the signature frame is collected",
                      m17_dispatch_stream_payload(opts, state, payload_bits, M17_STREAM_SIGNATURE_FN0, processed_bits),
                      M17_STREAM_SIGNATURE_CONSUMED);
    err |= expect_int("the signature frame plays nothing", g_codec2_decode_calls - decodes, 0);
    dsd_state_ext_free_all(state);
    return err;
}

/* An M17 call a fade ended (sync loss, recoverable) is finalized by its terminator when that is heard after all, so the
   station's next transmission is not merged into its row; another protocol's ended call is left alone. */
static int
test_m17_end_finalizes_a_sync_loss_end(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    int err = 0;
    static const int protocols[] = {DSD_SYNC_M17_STR_POS, DSD_SYNC_DMR_BS_VOICE_POS};
    for (size_t p = 0U; p < sizeof protocols / sizeof protocols[0]; p++) {
        ip_test_state(opts, state);
        const dsd_call_observation open = {
            .protocol = protocols[p],
            .slot = 0U,
            .kind = DSD_CALL_KIND_VOICE,
            .ota_target_id = 1201U,
            .ota_source_id = 4201U,
        };
        err |= expect_int("call opens", dsd_call_state_observe(state, &open, DSD_CALL_BOUNDARY_BEGIN), 1);
        err |= expect_int("a fade ends it", dsd_call_state_end_ex(state, 0U, 0.0, DSD_CALL_END_SYNC_LOSS), 1);
        m17_end_call(opts, state, DSD_CALL_END_TERMINATOR, 0);
        dsd_call_snapshot call;
        err |= expect_int("ended call record", get_m17_call(state, &call), 1);
        err |= expect_int(DSD_SYNC_IS_M17(protocols[p]) ? "the terminator finalizes the M17 end"
                                                        : "an M17 terminator leaves another protocol's end",
                          call.end_reason,
                          DSD_SYNC_IS_M17(protocols[p]) ? DSD_CALL_END_TERMINATOR : DSD_CALL_END_SYNC_LOSS);
    }
    dsd_state_ext_free_all(state);
    return err;
}

/* Gate round 2 (issue #890): the network path's one rule set, and the RF end of stream. */

/* A voice-3200 IP stream frame, clear, CAN 3, from M17_REF_LSF_SRC_CSD; @p reserved puts a reserved TYPE bit in. */
static void
build_voice_lsf(uint8_t lsf_bits[TEST_M17_LSF_BITS], uint8_t can, int reserved) {
    uint8_t lsf_packed[M17_LSF_BYTES];
    build_checked_lsf(lsf_bits, lsf_packed, m17_compose_frame_info(1U, 2U, 0U, 0U, can, 0U, reserved ? 1U : 0U), NULL);
}

/* Each frame number of a stream is used once and in order: a copy of a frame, or one arriving after a later one, plays
   nothing and marks no media. */
static int
test_ip_duplicate_frames_are_dropped(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t payload[M17_SIGNATURE_DIGEST_BYTES];
    int err = 0;

    ip_test_state(opts, state);
    build_voice_lsf(lsf_bits, 3U, 0);
    DSD_MEMSET(payload, 0x21, sizeof(payload));
    send_ip_stream_frame(opts, state, lsf_bits, 0x1111U, 0U, 0U, payload, 0);
    send_ip_stream_frame(opts, state, lsf_bits, 0x1111U, 1U, 0U, payload, 0);
    dsd_call_snapshot call;
    err |= expect_int("stream call", get_m17_call(state, &call), 1);
    const double media = call.media_updated_m;
    uint8_t digest_before[M17_SIGNATURE_DIGEST_BYTES];
    DSD_MEMCPY(digest_before, state->m17_signature_digest, sizeof(digest_before));
    const int decodes = g_codec2_decode_calls;
    send_ip_stream_frame(opts, state, lsf_bits, 0x1111U, 1U, 0U, payload, 0);
    send_ip_stream_frame(opts, state, lsf_bits, 0x1111U, 0U, 0U, payload, 0);
    err |= expect_int("a copy and a late frame play nothing", g_codec2_decode_calls - decodes, 0);
    err |= expect_int("they keep the call", get_m17_call(state, &call), 1);
    err |= expect_int("they mark no media", call.media_updated_m > media, 0);
    err |= expect_bytes("they enter no digest", state->m17_signature_digest, digest_before, sizeof(digest_before));
    err |= expect_u8("they leave the voice run whole", state->m17_signature_voice_gap, 0U);
    dsd_state_ext_free_all(state);
    return err;
}

/* A frame whose own link setup cannot be used (a reserved TYPE bit) is nothing, whether it opens another stream or
   continues the open one: the open stream's CAN, encryption and type never stand in for it. */
static int
test_ip_unusable_link_setup_dispatches_nothing(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t good[TEST_M17_LSF_BITS];
    uint8_t bad[TEST_M17_LSF_BITS];
    uint8_t payload[M17_SIGNATURE_DIGEST_BYTES];
    int err = 0;

    ip_test_state(opts, state);
    build_voice_lsf(good, 3U, 0);
    build_voice_lsf(bad, 4U, 1);
    DSD_MEMSET(payload, 0x31, sizeof(payload));
    send_ip_stream_frame(opts, state, good, 0x2222U, 0U, 0U, payload, 0);
    dsd_call_snapshot call;
    err |= expect_int("clear voice on CAN 3", get_m17_call(state, &call), 1);
    const uint64_t epoch = call.epoch;
    int decodes = g_codec2_decode_calls;
    send_ip_stream_frame(opts, state, bad, 0x3333U, 0U, 0U, payload, 0);
    err |= expect_int("an unusable new stream plays nothing", g_codec2_decode_calls - decodes, 0);
    err |= expect_u8("it keeps the CAN", state->m17_can, 3U);
    err |= expect_int("it keeps the open stream", (int)state->m17_ip_stream_sid, (int)0x2222U);
    err |= expect_int("it opens no call", get_m17_call(state, &call), 1);
    err |= expect_u64("it starts no epoch", call.epoch, epoch);
    decodes = g_codec2_decode_calls;
    send_ip_stream_frame(opts, state, bad, 0x2222U, 1U, 0U, payload, 0);
    err |= expect_int("an unusable frame of the open stream plays nothing", g_codec2_decode_calls - decodes, 0);
    decodes = g_codec2_decode_calls;
    send_ip_stream_frame(opts, state, good, 0x2222U, 1U, 0U, payload, 0);
#ifdef USE_CODEC2
    err |= expect_int("the open stream's next usable frame plays", g_codec2_decode_calls - decodes, 2);
#endif
    dsd_state_ext_free_all(state);
    return err;
}

/* EOTX and DISC end the open stream only when they name its station: a delayed one from another station leaves the
   stream playing. Its own station's EOTX ends it. */
static int
test_ip_end_controls_match_the_station(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t payload[M17_SIGNATURE_DIGEST_BYTES];
    uint8_t control[10];
    int err = 0;

    ip_test_state(opts, state);
    build_voice_lsf(lsf_bits, 3U, 0);
    DSD_MEMSET(payload, 0x41, sizeof(payload));
    send_ip_stream_frame(opts, state, lsf_bits, 0x4444U, 0U, 0U, payload, 0);
    build_ip_control(control, "EOTX", "N0OTHR");
    m17_ip_dispatch_frame(opts, state, control, sizeof(control));
    build_ip_control(control, "DISC", "N0OTHR");
    m17_ip_dispatch_frame(opts, state, control, sizeof(control));
    dsd_call_snapshot call;
    err |= expect_int("another station's end: call", get_m17_call(state, &call), 1);
    err |= expect_int("another station's end leaves the call", call.phase == DSD_CALL_PHASE_ACTIVE, 1);
    const int decodes = g_codec2_decode_calls;
    send_ip_stream_frame(opts, state, lsf_bits, 0x4444U, 1U, 0U, payload, 0);
#ifdef USE_CODEC2
    err |= expect_int("the stream plays on", g_codec2_decode_calls - decodes, 2);
#endif
    (void)decodes;
    build_ip_control(control, "EOTX", M17_REF_LSF_SRC_CSD);
    m17_ip_dispatch_frame(opts, state, control, sizeof(control));
    err |= expect_int("its station's EOTX: call", get_m17_call(state, &call), 1);
    err |= expect_int("its station's EOTX ends the call", call.phase == DSD_CALL_PHASE_ACTIVE, 0);
    err |= expect_int("its station's EOTX drops the carrier", state->carrier, 0);
    dsd_state_ext_free_all(state);
    return err;
}

/* After a stream ends, every later datagram of its stream ID is ignored, a late signature chunk included, until frame
   0 starts it again; a late EOT of an ended stream never touches the stream open since (A EOT, B frame 0, A EOT). */
static int
test_ip_ended_stream_is_ignored_until_frame_0(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t signed_lsf[TEST_M17_LSF_BITS];
    uint8_t voice_lsf[TEST_M17_LSF_BITS];
    uint8_t lsf_packed[M17_LSF_BYTES];
    uint8_t payloads[TEST_SIGNED_VOICE_FRAMES][M17_SIGNATURE_DIGEST_BYTES];
    int err = 0;

    ip_test_state(opts, state);
    build_checked_lsf(signed_lsf, lsf_packed, m17_compose_frame_info(1U, 2U, 0U, 0U, 3U, 1U, 0U), NULL);
    build_voice_lsf(voice_lsf, 3U, 0);
    build_signed_payloads(payloads, TEST_SIGNED_VOICE_FRAMES);
    const uint16_t a = 0x5555U;
    const uint16_t b = 0x6666U;
    for (int f = 0; f < TEST_SIGNED_VOICE_FRAMES; f++) {
        send_ip_stream_frame(opts, state, signed_lsf, a, (uint16_t)f, 0U, payloads[f], 0);
    }
    /* Chunks 0, 1 and the last (the EOT); chunk 2 is late. */
    static const size_t k_chunks[3] = {0U, 1U, 3U};
    for (size_t i = 0U; i < 3U; i++) {
        const uint16_t fn = k_test_signature_fns[k_chunks[i]];
        send_ip_stream_frame(opts, state, signed_lsf, a, (uint16_t)(fn & M17_STREAM_FRAME_COUNTER_MAX),
                             (uint8_t)((fn & M17_STREAM_FRAME_END_MASK) != 0U),
                             M17_REF_SIGNATURE_BYTES + (k_chunks[i] * 16U), 0);
    }
    err |= expect_int("A's EOT ended it", (int)state->m17_ip_stream, DSD_M17_IP_STREAM_NONE);
    const int decodes_late = g_codec2_decode_calls;
    send_ip_stream_frame(opts, state, voice_lsf, a, 3U, 0U, payloads[3], 0);
    err |= expect_int("a late voice frame after the end plays nothing", g_codec2_decode_calls - decodes_late, 0);
    err |=
        expect_int("a late voice frame after the end opens nothing", (int)state->m17_ip_stream, DSD_M17_IP_STREAM_NONE);

    send_ip_stream_frame(opts, state, voice_lsf, b, 0U, 0U, payloads[0], 0);
    dsd_call_snapshot call;
    err |= expect_int("stream B: call", get_m17_call(state, &call), 1);
    const uint64_t b_epoch = call.epoch;
    uint8_t b_digest[M17_SIGNATURE_DIGEST_BYTES];
    DSD_MEMCPY(b_digest, state->m17_signature_digest, sizeof(b_digest));
    const int decodes = g_codec2_decode_calls;
    send_ip_stream_frame(opts, state, signed_lsf, a,
                         (uint16_t)(M17_STREAM_SIGNATURE_FN3 & M17_STREAM_FRAME_COUNTER_MAX), 1U,
                         M17_REF_SIGNATURE_BYTES + 48U, 0);
    err |= expect_int("A's late EOT: call", get_m17_call(state, &call), 1);
    err |= expect_int("A's late EOT leaves B's call", call.phase == DSD_CALL_PHASE_ACTIVE, 1);
    err |= expect_u64("A's late EOT starts no epoch", call.epoch, b_epoch);
    err |= expect_int("A's late EOT leaves B open", (int)state->m17_ip_stream_sid, (int)b);
    err |= expect_bytes("A's late EOT leaves B's digest", state->m17_signature_digest, b_digest, sizeof(b_digest));
    err |= expect_int("A's late EOT plays nothing", g_codec2_decode_calls - decodes, 0);

    const int decodes_a = g_codec2_decode_calls;
    send_ip_stream_frame(opts, state, voice_lsf, a, 0U, 0U, payloads[0], 0);
    err |= expect_int("frame 0 starts A again", (int)state->m17_ip_stream_sid, (int)a);
    err |= expect_int("A again: call", get_m17_call(state, &call), 1);
    err |= expect_int("A again has its call", call.phase == DSD_CALL_PHASE_ACTIVE, 1);
#ifdef USE_CODEC2
    err |= expect_int("A again plays", g_codec2_decode_calls - decodes_a, 2);
#endif
    (void)decodes_a;
    dsd_state_ext_free_all(state);
    return err;
}

/* An RF signed transmission (LSF frame checked, signed, clear 3200 voice): its LSF frame, then @p frames voice frames
   from @p order (frame numbers, -1 to skip), then the signature chunks @p chunks (indexes into k_test_signature_fns,
   -1 to stop). */
static void
rf_signed_stream(dsd_opts* opts, dsd_state* state, uint8_t payloads[][M17_SIGNATURE_DIGEST_BYTES], const int* order,
                 size_t frames, const int* chunks, size_t nchunks) {
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t lsf_packed[M17_LSF_BYTES];
    uint8_t payload_bits[M17_STREAM_PAYLOAD_BITS];
    uint8_t processed_bits[M17_STREAM_PAYLOAD_BITS];
    uint16_t crc = 0U;
    build_checked_lsf(lsf_bits, lsf_packed, m17_compose_frame_info(1U, 2U, 0U, 0U, 3U, 1U, 0U), &crc);
    DSD_MEMCPY(state->m17_lsf, lsf_bits, sizeof(lsf_bits));
    (void)m17_finalize_lsf_crc(opts, state, lsf_packed, crc);
    for (size_t f = 0U; f < frames; f++) {
        if (order[f] < 0) {
            continue;
        }
        bytes_to_bits(payloads[order[f]], payload_bits, M17_SIGNATURE_DIGEST_BYTES);
        (void)m17_dispatch_stream_payload(opts, state, payload_bits, (uint16_t)order[f], processed_bits);
    }
    for (size_t i = 0U; i < nchunks && chunks[i] >= 0; i++) {
        expect_signature_slice(payload_bits, (size_t)chunks[i] * 16U);
        (void)m17_dispatch_stream_payload(opts, state, payload_bits, k_test_signature_fns[chunks[i]], processed_bits);
    }
}

/* An RF signed stream is judged only over every voice frame once and in order: one that lost an interior frame (even
   one whose payload is all zero, whose XOR into the digest is nothing), or played one twice (the digest takes a
   repeated frame number once, yet it plays), is incomplete, never "invalid" or "verified"; the whole stream verifies
   (M17 spec 2.0.11, Signature Verification). */
static int
test_signed_rf_stream_needs_every_frame_once(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t payloads[TEST_SIGNED_VOICE_FRAMES][M17_SIGNATURE_DIGEST_BYTES];
    int order[TEST_SIGNED_VOICE_FRAMES + 1];
    static const int k_all_chunks[4] = {0, 1, 2, 3};
    int err = 0;

    /* The signed payloads with frame 7 all zero, the last frame solved again for the reference digest. */
    build_signed_payloads(payloads, TEST_SIGNED_VOICE_FRAMES);
    DSD_MEMSET(payloads[7], 0, sizeof(payloads[7]));
    uint8_t digest[M17_SIGNATURE_DIGEST_BYTES];
    DSD_MEMSET(digest, 0, sizeof(digest));
    for (int f = 0; f + 1 < TEST_SIGNED_VOICE_FRAMES; f++) {
        m17_signature_digest_update(digest, payloads[f]);
    }
    for (size_t i = 0U; i < M17_SIGNATURE_DIGEST_BYTES; i++) {
        const size_t from = (i + M17_SIGNATURE_DIGEST_BYTES - 1U) % M17_SIGNATURE_DIGEST_BYTES;
        payloads[TEST_SIGNED_VOICE_FRAMES - 1][i] = (uint8_t)(M17_REF_SIGNATURE_DIGEST[from] ^ digest[i]);
    }

    for (int f = 0; f < TEST_SIGNED_VOICE_FRAMES; f++) {
        order[f] = (f == 7) ? -1 : f;
    }
    ip_test_state(opts, state);
    rf_signed_stream(opts, state, payloads, order, TEST_SIGNED_VOICE_FRAMES, k_all_chunks, 4U);
    err |= expect_u8("a stream missing a frame is incomplete", state->m17_signature_verification_status, 5U);

    /* Frame 0 played twice. */
    order[0] = 0;
    order[1] = 0;
    for (int f = 1; f < TEST_SIGNED_VOICE_FRAMES; f++) {
        order[f + 1] = f;
    }
    ip_test_state(opts, state);
    rf_signed_stream(opts, state, payloads, order, TEST_SIGNED_VOICE_FRAMES + 1, k_all_chunks, 4U);
    err |= expect_u8("a stream that played a frame twice is incomplete", state->m17_signature_verification_status, 5U);

    for (int f = 0; f < TEST_SIGNED_VOICE_FRAMES; f++) {
        order[f] = f;
    }
    ip_test_state(opts, state);
    rf_signed_stream(opts, state, payloads, order, TEST_SIGNED_VOICE_FRAMES, k_all_chunks, 4U);
    err |= expect_u8("the whole stream verifies", state->m17_signature_verification_status, 1U);
    dsd_state_ext_free_all(state);
    return err;
}

/* A signed RF transmission whose last chunk (with the EOS) was lost, ended by its EOT marker alone: the signature is
   reported incomplete before the marker's reset forgets it. */
static int
test_rf_marker_end_reports_an_incomplete_signature(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t payloads[TEST_SIGNED_VOICE_FRAMES][M17_SIGNATURE_DIGEST_BYTES];
    int order[TEST_SIGNED_VOICE_FRAMES];
    static const int k_three_chunks[3] = {0, 1, 2};
    int err = 0;

    build_signed_payloads(payloads, TEST_SIGNED_VOICE_FRAMES);
    for (int f = 0; f < TEST_SIGNED_VOICE_FRAMES; f++) {
        order[f] = f;
    }
    ip_test_state(opts, state);
    rf_signed_stream(opts, state, payloads, order, TEST_SIGNED_VOICE_FRAMES, k_three_chunks, 3U);
    err |= expect_u8("three chunks: not judged yet", state->m17_signature_verification_status, 0U);
    m17_note_transmission_end(state);
    err |= expect_u8("the marker reports it incomplete", state->m17_signature_verification_status, 5U);
    dsd_state_ext_free_all(state);
    return err;
}

/* An RF stream frame's EOS ends the transmission's signature: the next transmission, acquired from its stream frames
   with no EOT marker or LSF frame between, starts with an empty set, no verdict and no advertisement. */
static int
test_rf_eos_retires_the_signature(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t lsf_packed[M17_LSF_BYTES];
    uint8_t payloads[TEST_SIGNED_VOICE_FRAMES][M17_SIGNATURE_DIGEST_BYTES];
    uint8_t payload_bits[M17_STREAM_PAYLOAD_BITS];
    uint8_t processed_bits[M17_STREAM_PAYLOAD_BITS];
    uint16_t crc = 0U;
    int err = 0;

    ip_test_state(opts, state);
    build_checked_lsf(lsf_bits, lsf_packed, m17_compose_frame_info(1U, 2U, 0U, 0U, 3U, 1U, 0U), &crc);
    build_signed_payloads(payloads, TEST_SIGNED_VOICE_FRAMES);
    DSD_MEMCPY(state->m17_lsf, lsf_bits, sizeof(lsf_bits));
    err |= expect_int("A's LSF frame", m17_finalize_lsf_crc(opts, state, lsf_packed, crc), 0);
    for (int f = 0; f < TEST_SIGNED_VOICE_FRAMES; f++) {
        bytes_to_bits(payloads[f], payload_bits, M17_SIGNATURE_DIGEST_BYTES);
        (void)m17_dispatch_stream_payload(opts, state, payload_bits, (uint16_t)f, processed_bits);
    }
    for (size_t i = 0U; i < 4U; i++) {
        expect_signature_slice(payload_bits, i * 16U);
        (void)m17_dispatch_stream_payload(opts, state, payload_bits, k_test_signature_fns[i], processed_bits);
    }
    err |= expect_u8("A verifies", state->m17_signature_verification_status, 1U);
    m17_stream_end_of_stream(opts, state);
    err |= expect_u8("A's verdict stands to its end", state->m17_signature_verification_status, 1U);

    /* B's first stream frame (its LICH chunk 2, frame 5): A's signature is retired. */
    uint8_t encoded[M17_LICH_BITS];
    build_encoded_lich_chunk(lsf_bits, 2U, encoded);
    (void)m17_process_lich(state, opts, encoded);
    err |= expect_u8("B starts with no verdict", state->m17_signature_verification_status, 0U);
    err |= expect_u8("B starts with no chunks", state->m17_signature_received_mask, 0U);
    err |= expect_u8("B starts with no advertisement", state->m17_signature_advertised, 0U);
    err |= expect_int("B's voice run starts again", (int)state->m17_signature_voice_next, (int)0U);
    dsd_state_ext_free_all(state);
    return err;
}

/* Gate round 3 (issue #890). */

/* A voice IP frame of station @p callsign: build_voice_lsf()'s LSF with another source. */
static void
build_voice_lsf_from(uint8_t lsf_bits[TEST_M17_LSF_BITS], const char* callsign) {
    uint8_t lsf_packed[M17_LSF_BYTES];
    const unsigned long long dst = m17_encode_b40_callsign(0ULL, M17_REF_LSF_DST_CSD);
    const unsigned long long src = m17_encode_b40_callsign(0ULL, callsign);
    build_lsf_bits(lsf_bits, dst, src, m17_compose_frame_info(1U, 2U, 0U, 0U, 3U, 0U, 0U), NULL);
    (void)m17_attach_lsf_crc(lsf_bits, lsf_packed);
}

/* A final frame (EOT) of an unseen stream, not frame 0, does not replace the open stream: A plays on. */
static int
test_ip_foreign_eot_does_not_end_the_open_stream(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t payload[M17_SIGNATURE_DIGEST_BYTES];
    int err = 0;
    ip_test_state(opts, state);
    build_voice_lsf(lsf_bits, 3U, 0);
    DSD_MEMSET(payload, 0x51, sizeof(payload));
    send_ip_stream_frame(opts, state, lsf_bits, 0xA000U, 0U, 0U, payload, 0);
    send_ip_stream_frame(opts, state, lsf_bits, 0xB000U, 15U, 1U, payload, 0);
    err |= expect_int("A stays open", (int)state->m17_ip_stream_sid, 0xA000);
    err |= expect_int("A stays open (state)", (int)state->m17_ip_stream, DSD_M17_IP_STREAM_OPEN);
    dsd_call_snapshot call;
    err |= expect_int("A's call", get_m17_call(state, &call), 1);
    err |= expect_int("A's call stays active", call.phase == DSD_CALL_PHASE_ACTIVE, 1);
    const int decodes = g_codec2_decode_calls;
    send_ip_stream_frame(opts, state, lsf_bits, 0xA000U, 1U, 0U, payload, 0);
#ifdef USE_CODEC2
    err |= expect_int("A plays on", g_codec2_decode_calls - decodes, 2);
#endif
    (void)decodes;
    dsd_state_ext_free_all(state);
    return err;
}

/* The 15-bit frame counter wraps from 0x7FFF to 0 on a long unsigned stream: the frames after the wrap play, a late
   copy from before it does not. */
static int
test_ip_frame_counter_rollover(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t payload[M17_SIGNATURE_DIGEST_BYTES];
    int err = 0;
    ip_test_state(opts, state);
    build_voice_lsf(lsf_bits, 3U, 0);
    DSD_MEMSET(payload, 0x52, sizeof(payload));
    send_ip_stream_frame(opts, state, lsf_bits, 0xC000U, 0x7FFEU, 0U, payload, 0);
    send_ip_stream_frame(opts, state, lsf_bits, 0xC000U, 0x7FFFU, 0U, payload, 0);
    int decodes = g_codec2_decode_calls;
    send_ip_stream_frame(opts, state, lsf_bits, 0xC000U, 0U, 0U, payload, 0);
    send_ip_stream_frame(opts, state, lsf_bits, 0xC000U, 1U, 0U, payload, 0);
#ifdef USE_CODEC2
    err |= expect_int("the frames after the wrap play", g_codec2_decode_calls - decodes, 4);
#endif
    err |= expect_int("the wrap keeps the stream", (int)state->m17_ip_stream_last_fn, 1);
    decodes = g_codec2_decode_calls;
    send_ip_stream_frame(opts, state, lsf_bits, 0xC000U, 0x7FFFU, 0U, payload, 0);
    err |= expect_int("a copy from before the wrap plays nothing", g_codec2_decode_calls - decodes, 0);
    dsd_state_ext_free_all(state);
    return err;
}

/* A stream's in-band end (its EOT frame) drops the carrier and the sync type, as EOTX does. */
static int
test_ip_in_band_end_drops_the_carrier(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t payload[M17_SIGNATURE_DIGEST_BYTES];
    int err = 0;
    ip_test_state(opts, state);
    build_voice_lsf(lsf_bits, 3U, 0);
    DSD_MEMSET(payload, 0x53, sizeof(payload));
    send_ip_stream_frame(opts, state, lsf_bits, 0xD000U, 0U, 0U, payload, 0);
    err |= expect_int("an open stream holds the carrier", state->carrier, 1);
    send_ip_stream_frame(opts, state, lsf_bits, 0xD000U, 1U, 1U, payload, 0);
    err |= expect_int("its EOT drops the carrier", state->carrier, 0);
    err |= expect_int("its EOT clears the sync type", state->synctype, DSD_SYNC_NONE);
    dsd_state_ext_free_all(state);
    return err;
}

/* A stream another replaces (its end lost) ends its call first: the same station and destination on a new stream ID
   are two calls, not one. */
static int
test_ip_replaced_stream_ends_its_call(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t payload[M17_SIGNATURE_DIGEST_BYTES];
    int err = 0;
    ip_test_state(opts, state);
    build_voice_lsf(lsf_bits, 3U, 0);
    DSD_MEMSET(payload, 0x54, sizeof(payload));
    send_ip_stream_frame(opts, state, lsf_bits, 0xE000U, 0U, 0U, payload, 0);
    dsd_call_snapshot call;
    err |= expect_int("A's call", get_m17_call(state, &call), 1);
    const uint64_t a_epoch = call.epoch;
    send_ip_stream_frame(opts, state, lsf_bits, 0xE001U, 0U, 0U, payload, 0);
    err |= expect_int("B's call", get_m17_call(state, &call), 1);
    err |= expect_int("B is another call", call.epoch != a_epoch, 1);
    err |= expect_int("B's call is active", call.phase == DSD_CALL_PHASE_ACTIVE, 1);
    dsd_state_ext_free_all(state);
    return err;
}

/* An ended stream is its stream ID and station: when station B takes over A's stream ID, a late frame of A is dropped
   and B plays on. Ended streams are kept while others end meanwhile, up to the table's 64 keys
   (test_ip_ended_streams_are_bounded_and_fail_closed()). */
static int
test_ip_ended_streams_by_station(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_a[TEST_M17_LSF_BITS];
    uint8_t lsf_b[TEST_M17_LSF_BITS];
    uint8_t payload[M17_SIGNATURE_DIGEST_BYTES];
    int err = 0;
    ip_test_state(opts, state);
    build_voice_lsf(lsf_a, 3U, 0);
    build_voice_lsf_from(lsf_b, "N0OTHR");
    DSD_MEMSET(payload, 0x55, sizeof(payload));
    send_ip_stream_frame(opts, state, lsf_a, 0xF000U, 1U, 0U, payload, 0);
    send_ip_stream_frame(opts, state, lsf_b, 0xF000U, 0U, 0U, payload, 0);
    int decodes = g_codec2_decode_calls;
    send_ip_stream_frame(opts, state, lsf_a, 0xF000U, 1U, 0U, payload, 0);
    err |= expect_int("A's late frame plays nothing", g_codec2_decode_calls - decodes, 0);
    err |= expect_int("B stays open", state->m17_ip_stream_src == m17_encode_b40_callsign(0ULL, "N0OTHR"), 1);

    /* Station A's ended stream is A's: station B joining a stream on the same ID mid-way (frame 7) is B's own and
       plays. */
    send_ip_stream_frame(opts, state, lsf_b, 0xF000U, 1U, 1U, payload, 0);
    uint8_t lsf_c[TEST_M17_LSF_BITS];
    build_voice_lsf_from(lsf_c, "N0THRD");
    send_ip_stream_frame(opts, state, lsf_a, 0xF100U, 0U, 1U, payload, 0);
    decodes = g_codec2_decode_calls;
    send_ip_stream_frame(opts, state, lsf_c, 0xF100U, 7U, 0U, payload, 0);
#ifdef USE_CODEC2
    err |= expect_int("another station on an ended stream's ID plays", g_codec2_decode_calls - decodes, 2);
#endif
    err |=
        expect_int("another station on an ended stream's ID opens", (int)state->m17_ip_stream, DSD_M17_IP_STREAM_OPEN);
    send_ip_stream_frame(opts, state, lsf_c, 0xF100U, 8U, 1U, payload, 0);

    /* 20 more streams end; the first ended one is still ignored until its frame 0. */
    for (uint16_t sid = 0x0100U; sid < 0x0114U; sid++) {
        send_ip_stream_frame(opts, state, lsf_a, sid, 0U, 1U, payload, 0);
    }
    decodes = g_codec2_decode_calls;
    send_ip_stream_frame(opts, state, lsf_b, 0xF000U, 2U, 0U, payload, 0);
    err |= expect_int("an ended stream stays ignored after 20 others", g_codec2_decode_calls - decodes, 0);
    err |= expect_int("an ended stream opens nothing", (int)state->m17_ip_stream, DSD_M17_IP_STREAM_NONE);
    dsd_state_ext_free_all(state);
    return err;
}

/* A stream datagram whose LSF is packet mode is nothing (packet traffic is MPKT): it opens no call, evicts no stream,
   and does not take the frame number, so the corrected voice frame with that number plays. */
static int
test_ip_packet_mode_lsf_in_a_stream_datagram(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t voice[TEST_M17_LSF_BITS];
    uint8_t packet[TEST_M17_LSF_BITS];
    uint8_t lsf_packed[M17_LSF_BYTES];
    uint8_t payload[M17_SIGNATURE_DIGEST_BYTES];
    int err = 0;
    ip_test_state(opts, state);
    build_voice_lsf(voice, 3U, 0);
    build_checked_lsf(packet, lsf_packed, m17_compose_frame_info(0U, 0U, 0U, 0U, 3U, 0U, 0U), NULL);
    DSD_MEMSET(payload, 0x56, sizeof(payload));
    send_ip_stream_frame(opts, state, voice, 0x1200U, 0U, 0U, payload, 0);
    dsd_call_snapshot call;
    err |= expect_int("voice call", get_m17_call(state, &call), 1);
    const uint64_t epoch = call.epoch;
    send_ip_stream_frame(opts, state, packet, 0x1200U, 1U, 0U, payload, 0);
    send_ip_stream_frame(opts, state, packet, 0x1300U, 0U, 0U, payload, 0);
    err |= expect_int("a packet LSF opens no call", get_m17_call(state, &call), 1);
    err |= expect_u64("a packet LSF starts no epoch", call.epoch, epoch);
    err |= expect_int("a packet LSF evicts no stream", (int)state->m17_ip_stream_sid, 0x1200);
    const int decodes = g_codec2_decode_calls;
    send_ip_stream_frame(opts, state, voice, 0x1200U, 1U, 0U, payload, 0);
#ifdef USE_CODEC2
    err |= expect_int("the voice frame with that number plays", g_codec2_decode_calls - decodes, 2);
#endif
    (void)decodes;
    dsd_state_ext_free_all(state);
    return err;
}

/* A network frame's decoded speech marks the canonical call's media. */
static int
test_ip_decoded_speech_marks_media(void) {
    int err = 0;
#ifdef USE_CODEC2
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t payload[M17_SIGNATURE_DIGEST_BYTES];
    ip_test_state(opts, state);
    build_voice_lsf(lsf_bits, 3U, 0);
    DSD_MEMSET(payload, 0x57, sizeof(payload));
    send_ip_stream_frame(opts, state, lsf_bits, 0x1400U, 0U, 0U, payload, 0);
    dsd_call_snapshot call;
    err |= expect_int("IP voice call", get_m17_call(state, &call), 1);
    err |= expect_int("decoded IP speech marks media", call.media_active, 1);
    dsd_state_ext_free_all(state);
#endif
    return err;
}

/* -F shows a CRC-failed AES LSF without applying it: its trailer prints the nonce it carries, not the bound IV. */
static int
test_relaxed_lsf_logs_its_own_iv(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t checked_bits[TEST_M17_LSF_BITS];
    uint8_t checked_packed[M17_LSF_BYTES];
    uint8_t relaxed_bits[TEST_M17_LSF_BITS];
    uint8_t relaxed_packed[M17_LSF_BYTES];
    uint16_t crc = 0U;
    int err = 0;
    ip_test_state(opts, state);
    opts->aggressive_framesync = 0;
    build_checked_lsf(checked_bits, checked_packed, m17_compose_frame_info(1U, 2U, 0U, 0U, 3U, 0U, 0U), &crc);
    DSD_MEMCPY(state->m17_lsf, checked_bits, sizeof(checked_bits));
    (void)m17_finalize_lsf_crc(opts, state, checked_packed, crc);
    const unsigned long long dst = m17_encode_b40_callsign(0ULL, M17_REF_LSF_DST_CSD);
    const unsigned long long src = m17_encode_b40_callsign(0ULL, M17_REF_LSF_SRC_CSD);
    build_lsf_bits(relaxed_bits, dst, src, m17_compose_frame_info(1U, 2U, 2U, 0U, 3U, 0U, 0U), M17_REF_AES_NONCE);
    (void)m17_attach_lsf_crc(relaxed_bits, relaxed_packed);
    relaxed_bits[M17_LSF_LSD_BITS + 2U] ^= 1U;
    dsd_test_capture_stderr cap;
    char out[8192];
    if (dsd_test_capture_stderr_begin(&cap, "m17_iv") != 0) {
        return 1;
    }
    for (uint8_t chunk = 0U; chunk < M17_LICH_CHUNKS; chunk++) {
        (void)feed_lich_frame(opts, state, relaxed_bits, chunk);
    }
    (void)dsd_test_capture_stderr_end(&cap);
    (void)dsd_test_capture_stderr_read(&cap, out, sizeof(out));
    err |= expect_int("the relaxed LSF is shown, not applied", state->m17_enc, 0);
    err |= expect_int("its trailer prints its own nonce", strstr(out, " IV: 000102030405060708090A0B0C0D") != NULL, 1);
    dsd_state_ext_free_all(state);
    return err;
}

/* After a decoded EOS with its EOT marker missed, the next transmission does not run on the ended one's binding: its
   first LICH retires the confirmation and the checked link setup, and a CRC-failed LSF frame of it is decoded under
   -F, not refused as a repeat of the ended one's. */
static int
test_rf_eos_retires_the_link_setup(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t lsf_packed[M17_LSF_BYTES];
    uint8_t other_bits[TEST_M17_LSF_BITS];
    uint8_t other_packed[M17_LSF_BYTES];
    uint16_t crc = 0U;
    uint16_t other_crc = 0U;
    int err = 0;
    ip_test_state(opts, state);
    opts->aggressive_framesync = 0;
    build_checked_lsf(lsf_bits, lsf_packed, m17_compose_frame_info(1U, 2U, 0U, 0U, 3U, 1U, 0U), &crc);
    const unsigned long long dst = m17_encode_b40_callsign(0ULL, M17_REF_LSF_DST_CSD);
    const unsigned long long src = m17_encode_b40_callsign(0ULL, M17_REF_LSF_SRC_CSD);
    build_lsf_bits(other_bits, dst, src, m17_compose_frame_info(1U, 3U, 2U, 0U, 9U, 0U, 0U), M17_REF_AES_NONCE);
    other_crc = m17_attach_lsf_crc(other_bits, other_packed);

    /* The LICH path. */
    DSD_MEMCPY(state->m17_lsf, lsf_bits, sizeof(lsf_bits));
    err |= expect_int("A's LSF frame", m17_finalize_lsf_crc(opts, state, lsf_packed, crc), 0);
    err |= expect_int("A is confirmed", m17_confirm_is_confirmed(state), 1);
    m17_stream_end_of_stream(opts, state);
    err |= expect_int("A's evidence stands for its EOT marker", m17_confirm_is_confirmed(state), 1);
    uint8_t encoded[M17_LICH_BITS];
    build_encoded_lich_chunk(lsf_bits, 3U, encoded);
    (void)m17_process_lich(state, opts, encoded);
    err |= expect_u8("B's first LICH retires the checked link setup", state->m17_lsf_checked, 0U);
    err |= expect_int("B's first LICH retires the confirmation", m17_confirm_is_confirmed(state), 0);
    err |= expect_u8("B's first LICH retires the advertisement", state->m17_signature_advertised, 0U);

    /* The LSF frame path: B's LSF frame fails its CRC; under -F it is decoded as B's. */
    ip_test_state(opts, state);
    opts->aggressive_framesync = 0;
    DSD_MEMCPY(state->m17_lsf, lsf_bits, sizeof(lsf_bits));
    (void)m17_finalize_lsf_crc(opts, state, lsf_packed, crc);
    m17_stream_end_of_stream(opts, state);
    DSD_MEMCPY(state->m17_lsf, other_bits, sizeof(other_bits));
    err |= expect_int("B's LSF frame fails", m17_finalize_lsf_crc(opts, state, other_packed, other_crc ^ 1U), 1);
    err |= expect_u8("B's relaxed LSF is B's CAN", state->m17_can, 9U);
    err |= expect_u8("B's relaxed LSF is B's encryption", state->m17_enc, 2U);
    err |= expect_u8("B is not A's signed transmission", state->m17_signature_advertised, 0U);
    dsd_state_ext_free_all(state);
    return err;
}

/* Gate round 4 (issue #890): the boundaries that retire a transmission's link setup, the signature range of a signed
   stream and the network rule set's last gaps. */

enum { TEST_STREAM_TYPE_UNKNOWN = 9 };

/* One RF stream frame's LICH chunk @p counter of @p lsf_bits through the frame's evidence accounting as
   processM17STR() runs it: a clean LICH is weak evidence. */
static int
feed_stream_lich(dsd_opts* opts, dsd_state* state, const uint8_t lsf_bits[TEST_M17_LSF_BITS], uint8_t counter) {
    uint8_t encoded[M17_LICH_BITS];
    build_encoded_lich_chunk(lsf_bits, counter, encoded);
    m17_confirm_begin_frame(state);
    const int rc = m17_process_lich(state, opts, encoded);
    if (rc == 0) {
        m17_confirm_note_evidence(opts, state, M17_EVIDENCE_WEAK);
    }
    m17_confirm_end_frame(state);
    return rc;
}

/* The LSF of an AES-encrypted 3200 voice stream on CAN 9 with a nonce, CRC attached. */
static uint16_t
build_encrypted_lsf(uint8_t lsf_bits[TEST_M17_LSF_BITS], uint8_t lsf_packed[M17_LSF_BYTES], uint8_t signature) {
    const unsigned long long dst = m17_encode_b40_callsign(0ULL, M17_REF_LSF_DST_CSD);
    const unsigned long long src = m17_encode_b40_callsign(0ULL, M17_REF_LSF_SRC_CSD);
    build_lsf_bits(lsf_bits, dst, src, m17_compose_frame_info(1U, 2U, 2U, 0U, 9U, signature, 0U), M17_REF_AES_NONCE);
    return m17_attach_lsf_crc(lsf_bits, lsf_packed);
}

/* What a boundary leaves of the decoded link setup: all of it unknown, the stream type the "no stream" value noCarrier()
   has always left. */
static int
expect_link_setup_unknown(const char* boundary, const dsd_state* state) {
    uint8_t zero_meta[sizeof(state->m17_meta)];
    DSD_MEMSET(zero_meta, 0, sizeof(zero_meta));
    const m17_state_ext* link = m17_state_ext_find(state);
    uint8_t zero_head[sizeof(link->lsf_checked_head)];
    DSD_MEMSET(zero_head, 0, sizeof(zero_head));
    int err = 0;
    if (state->m17_lsf_checked != 0U || state->m17_str_dt != (uint8_t)TEST_STREAM_TYPE_UNKNOWN || state->m17_can != 0U
        || state->m17_enc != 0U || state->m17_enc_st != 0U || state->m17_payload_decrypted != 0U
        || state->m17_signature_advertised != 0U || memcmp(state->m17_meta, zero_meta, sizeof(zero_meta)) != 0) {
        DSD_FPRINTF(stderr,
                    "%s: link setup left: checked %u type %u CAN %u enc %u/%u decrypted %u signed %u meta[0] %02X\n",
                    boundary, (unsigned)state->m17_lsf_checked, (unsigned)state->m17_str_dt, (unsigned)state->m17_can,
                    (unsigned)state->m17_enc, (unsigned)state->m17_enc_st, (unsigned)state->m17_payload_decrypted,
                    (unsigned)state->m17_signature_advertised, (unsigned)state->m17_meta[0]);
        err = 1;
    }
    /* The scan side's copy of the checked LSF and its dispute go with it (issue #890). */
    if (link != NULL
        && (link->lsf_disputed != 0U || link->lsf_reconfirmed != 0U
            || memcmp(link->lsf_checked_head, zero_head, sizeof(zero_head)) != 0)) {
        DSD_FPRINTF(stderr, "%s: scan binding left: disputed %u reconfirmed %u head[0] %02X\n", boundary,
                    (unsigned)link->lsf_disputed, (unsigned)link->lsf_reconfirmed, (unsigned)link->lsf_checked_head[0]);
        err = 1;
    }
    return err;
}

/* A clear CAN 3 transmission A ends at its decoded EOS, its EOT marker and B's LSF frame missed. B, encrypted on CAN 9,
   is acquired from its LICH alone: two clean chunks confirm it before its LSF is rebuilt. B plays nothing, opens no
   call and earns no media under A's setup (issue #890); its own rebuilt LSF sets B up. */
static int
test_rf_eos_retires_the_decoded_link_setup(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t a_bits[TEST_M17_LSF_BITS];
    uint8_t a_packed[M17_LSF_BYTES];
    uint8_t b_bits[TEST_M17_LSF_BITS];
    uint8_t b_packed[M17_LSF_BYTES];
    uint8_t payload[M17_SIGNATURE_DIGEST_BYTES];
    uint8_t payload_bits[M17_STREAM_PAYLOAD_BITS];
    uint8_t processed_bits[M17_STREAM_PAYLOAD_BITS];
    uint16_t a_crc = 0U;
    int err = 0;

    ip_test_state(opts, state);
    build_checked_lsf(a_bits, a_packed, m17_compose_frame_info(1U, 2U, 0U, 0U, 3U, 0U, 0U), &a_crc);
    (void)build_encrypted_lsf(b_bits, b_packed, 0U);
    DSD_MEMSET(payload, 0x6B, sizeof(payload));
    bytes_to_bits(payload, payload_bits, sizeof(payload));

    DSD_MEMCPY(state->m17_lsf, a_bits, sizeof(a_bits));
    err |= expect_int("A's LSF frame", m17_finalize_lsf_crc(opts, state, a_packed, a_crc), 0);
    int decodes = g_codec2_decode_calls;
    err |= expect_int("A's voice is clear", m17_dispatch_stream_payload(opts, state, payload_bits, 0U, processed_bits),
                      M17_STREAM_CLEAR_DISPATCHED);
#ifdef USE_CODEC2
    err |= expect_int("A plays", g_codec2_decode_calls - decodes, 2);
#endif
    m17_stream_end_of_stream(opts, state);
    dsd_call_snapshot call;
    err |= expect_int("A's call record", get_m17_call(state, &call), 1);
    err |= expect_int("A's EOS ends its call", call.phase == DSD_CALL_PHASE_ACTIVE, 0);
    const uint64_t a_epoch = call.epoch;

    err |= expect_int("B's first chunk", feed_stream_lich(opts, state, b_bits, 0U), 0);
    err |= expect_int("B's second chunk", feed_stream_lich(opts, state, b_bits, 1U), 0);
    err |= expect_int("two clean chunks confirm B", m17_confirm_is_confirmed(state), 1);
    err |= expect_link_setup_unknown("B acquired from its LICH", state);
    decodes = g_codec2_decode_calls;
    (void)m17_dispatch_stream_payload(opts, state, payload_bits, 2U, processed_bits);
    err |= expect_int("B plays nothing under A's setup", g_codec2_decode_calls - decodes, 0);
    err |= expect_int("B's call record", get_m17_call(state, &call), 1);
    err |= expect_int("B opens no call under A's setup", call.phase == DSD_CALL_PHASE_ACTIVE, 0);
    err |= expect_u64("B starts no epoch under A's setup", call.epoch, a_epoch);

    /* B's own LSF, rebuilt whole from one LICH cycle heard under one binding, sets B up. */
    static const uint8_t k_cycle[] = {2U, 3U, 4U, 5U, 0U, 1U, 2U, 3U, 4U, 5U};
    for (size_t i = 0U; i < sizeof(k_cycle); i++) {
        (void)feed_stream_lich(opts, state, b_bits, k_cycle[i]);
    }
    err |= expect_u8("B's rebuilt LSF is checked", state->m17_lsf_checked, 1U);
    err |= expect_u8("B's rebuilt LSF sets its CAN", state->m17_can, 9U);
    err |= expect_u8("B's rebuilt LSF sets its encryption", state->m17_enc, 2U);
    err |= expect_u8("B's rebuilt LSF sets its type", state->m17_str_dt, 2U);
    err |= expect_int("B's rebuilt LSF opens its call", get_m17_call(state, &call), 1);
    err |= expect_int("B's call is another epoch", call.epoch != a_epoch, 1);
    decodes = g_codec2_decode_calls;
    err |= expect_int("B without its key stays locked",
                      m17_dispatch_stream_payload(opts, state, payload_bits, 9U, processed_bits),
                      M17_STREAM_ENCRYPTED_LOCKED);
    err |= expect_int("B without its key plays nothing", g_codec2_decode_calls - decodes, 0);
    dsd_state_ext_free_all(state);
    return err;
}

/* Every boundary that ends a transmission drops everything held from it through one helper (m17_confirm_reset()): the
   decoded link setup, the raw LSF bits and the LICH chunks held in them, the signature and the text being assembled. The
   boundaries: a decoded EOS and a packet's end (at the next transmission's first frame), the EOT marker, a lost carrier
   or a retune, a network stream's end. */
static int
test_link_setup_boundaries_clear_the_same_set(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t lsf_packed[M17_LSF_BYTES];
    uint8_t payload[M17_SIGNATURE_DIGEST_BYTES];
    int err = 0;
    const uint16_t crc = build_encrypted_lsf(lsf_bits, lsf_packed, 1U);
    DSD_MEMSET(payload, 0x6C, sizeof(payload));

    for (int boundary = 0; boundary < 6; boundary++) {
        ip_test_state(opts, state);
        if (boundary < 3 || boundary == 5) {
            DSD_MEMCPY(state->m17_lsf, lsf_bits, sizeof(lsf_bits));
            err |= expect_int("checked LSF frame", m17_finalize_lsf_crc(opts, state, lsf_packed, crc), 0);
        } else {
            send_ip_stream_frame(opts, state, lsf_bits, 0x7A00U, 0U, 0U, payload, 0);
        }
        err |= expect_u8("the LSF set the link up", state->m17_enc, 2U);
        seed_text_meta(state);
        /* A LICH chunk held under the transmission's binding. */
        uint8_t held[M17_LICH_BITS];
        build_encoded_lich_chunk(lsf_bits, 1U, held);
        (void)m17_process_lich(state, opts, held);
        /* A binding a scan hold already counts as disputed (another transmission's LICH, issue #890). */
        m17_state_ext* link = m17_state_ext_find(state);
        if (link != NULL) {
            link->lsf_disputed = 1U;
        }
        const char* label;
        switch (boundary) {
            case 0: {
                label = "an EOS, at the next frame's LICH";
                m17_stream_end_of_stream(opts, state);
                uint8_t encoded[M17_LICH_BITS];
                build_encoded_lich_chunk(lsf_bits, 4U, encoded);
                (void)m17_process_lich(state, opts, encoded);
                break;
            }
            case 1:
                /* The EOT marker as dsd_dispatch_handle_m17() ends the transmission; a lost carrier and a retune run the
                   same reset (no_carrier_reset_decode_state(), carrier_boundary_forget_evidence()). */
                label = "the EOT marker";
                m17_note_transmission_end(state);
                m17_confirm_reset(state);
                break;
            case 2:
                label = "a lost carrier or a retune";
                m17_confirm_reset(state);
                break;
            case 3:
                label = "a network stream's EOT";
                send_ip_stream_frame(opts, state, lsf_bits, 0x7A00U, 1U, 1U, payload, 0);
                break;
            case 4: {
                label = "a network stream's EOTX";
                uint8_t eotx[10];
                build_ip_control(eotx, "EOTX", M17_REF_LSF_SRC_CSD);
                m17_ip_dispatch_frame(opts, state, eotx, sizeof(eotx));
                break;
            }
            default: {
                label = "a packet's end, at the next frame's LICH";
                uint8_t app[2U + M17_META_BYTES];
                build_packet_text_meta_app(app, "PACKET-END-XX");
                stage_packet_with_crc(state, app, (uint16_t)sizeof(app), m17_crc16(app, (uint16_t)sizeof(app)));
                m17_pkt_finalize_eot(opts, state, (uint16_t)sizeof(app), (int)(sizeof(app) + M17_PACKET_CRC_BYTES));
                uint8_t encoded[M17_LICH_BITS];
                build_encoded_lich_chunk(lsf_bits, 4U, encoded);
                (void)m17_process_lich(state, opts, encoded);
                break;
            }
        }
        err |= expect_link_setup_unknown(label, state);
        /* The raw LSF bits and LICH chunks: at a next frame's LICH only its own chunk is held, elsewhere none. */
        const int next_chunk = boundary == 0 || boundary == 5;
        err |= expect_u8(label, state->m17_lich.held, (uint8_t)(next_chunk ? (1U << 4U) : 0U));
        for (size_t i = 0U; i < sizeof(state->m17_lsf); i++) {
            const int in_next = next_chunk && i >= (4U * M17_LICH_CHUNK_BITS) && i < (5U * M17_LICH_CHUNK_BITS);
            if (!in_next && state->m17_lsf[i] != 0U) {
                DSD_FPRINTF(stderr, "%s: LSF bit %zu kept from the ended transmission\n", label, i);
                err = 1;
                break;
            }
        }
        err |= expect_u8(label, text_meta_received(state), 0U);
        err |= expect_u8(label, state->m17_signature_verification_status, 0U);
    }
    dsd_state_ext_free_all(state);
    return err;
}

/* In a signed RF voice stream no voice takes a number in the signature range (M17 spec 2.0.11, Digital Signatures:
   signature use shortens the stream by four frames, whose numbers are 0x7FFC, 0x7FFD, 0x7FFE and 0xFFFF). A frame there
   that is no signature chunk (0x7FFF without the end bit, or another signature number with it) is never played and
   leaves the signature incomplete; so does a voice frame played from that range before the stream was known signed. */
static int
test_signed_rf_signature_range_frames(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t payloads[TEST_SIGNED_VOICE_FRAMES][M17_SIGNATURE_DIGEST_BYTES];
    uint8_t payload_bits[M17_STREAM_PAYLOAD_BITS];
    uint8_t processed_bits[M17_STREAM_PAYLOAD_BITS];
    int order[TEST_SIGNED_VOICE_FRAMES];
    static const int k_first_three[3] = {0, 1, 2};
    int err = 0;
    build_signed_payloads(payloads, TEST_SIGNED_VOICE_FRAMES);
    for (int f = 0; f < TEST_SIGNED_VOICE_FRAMES; f++) {
        order[f] = f;
    }
    uint8_t junk[M17_SIGNATURE_DIGEST_BYTES];
    DSD_MEMSET(junk, 0x3D, sizeof(junk));
    bytes_to_bits(junk, payload_bits, sizeof(junk));

    /* 0x7FFF without its end bit between the third chunk and the last. */
    ip_test_state(opts, state);
    rf_signed_stream(opts, state, payloads, order, TEST_SIGNED_VOICE_FRAMES, k_first_three, 3U);
    int decodes = g_codec2_decode_calls;
    err |= expect_int("0x7FFF without the end bit is no voice",
                      m17_dispatch_stream_payload(opts, state, payload_bits, 0x7FFFU, processed_bits),
                      M17_STREAM_SIGNATURE_CONSUMED);
    err |= expect_int("0x7FFF without the end bit plays nothing", g_codec2_decode_calls - decodes, 0);
    expect_signature_slice(payload_bits, 48U);
    (void)m17_dispatch_stream_payload(opts, state, payload_bits, M17_STREAM_SIGNATURE_FN3, processed_bits);
    err |= expect_u8("a malformed signature-range frame leaves the signature incomplete",
                     state->m17_signature_verification_status, 5U);

    /* 0xFFFD: a signature number with the end bit, after two chunks: the transmission's end, never voice. */
    static const int k_first_two[2] = {0, 1};
    ip_test_state(opts, state);
    rf_signed_stream(opts, state, payloads, order, TEST_SIGNED_VOICE_FRAMES, k_first_two, 2U);
    bytes_to_bits(junk, payload_bits, sizeof(junk));
    decodes = g_codec2_decode_calls;
    err |= expect_int("0xFFFD is no voice",
                      m17_dispatch_stream_payload(opts, state, payload_bits, 0xFFFDU, processed_bits),
                      M17_STREAM_SIGNATURE_CONSUMED);
    err |= expect_int("0xFFFD plays nothing", g_codec2_decode_calls - decodes, 0);
    m17_stream_end_of_stream(opts, state);
    err |= expect_u8("0xFFFD ends the transmission incomplete", state->m17_signature_verification_status, 5U);

    /* A stream whose LSF frame named no signature plays a frame numbered 0x7FFF; its LSF rebuilt from the LICH then
       advertises the signature, and every chunk arrives: the played frame is not in the digest, so the stream is
       incomplete, never verified. */
    uint8_t unsigned_bits[TEST_M17_LSF_BITS];
    uint8_t unsigned_packed[M17_LSF_BYTES];
    uint8_t signed_bits[TEST_M17_LSF_BITS];
    uint8_t signed_packed[M17_LSF_BYTES];
    uint16_t crc = 0U;
    build_checked_lsf(unsigned_bits, unsigned_packed, m17_compose_frame_info(1U, 2U, 0U, 0U, 3U, 0U, 0U), &crc);
    build_checked_lsf(signed_bits, signed_packed, m17_compose_frame_info(1U, 2U, 0U, 0U, 3U, 1U, 0U), NULL);
    ip_test_state(opts, state);
    DSD_MEMCPY(state->m17_lsf, unsigned_bits, sizeof(unsigned_bits));
    err |= expect_int("unsigned LSF frame", m17_finalize_lsf_crc(opts, state, unsigned_packed, crc), 0);
    for (int f = 0; f < TEST_SIGNED_VOICE_FRAMES; f++) {
        bytes_to_bits(payloads[f], payload_bits, M17_SIGNATURE_DIGEST_BYTES);
        (void)m17_dispatch_stream_payload(opts, state, payload_bits, (uint16_t)f, processed_bits);
    }
    bytes_to_bits(junk, payload_bits, sizeof(junk));
    (void)m17_dispatch_stream_payload(opts, state, payload_bits, 0x7FFFU, processed_bits);
    for (uint8_t chunk = 0U; chunk < M17_LICH_CHUNKS; chunk++) {
        (void)feed_lich_frame(opts, state, signed_bits, chunk);
    }
    err |= expect_u8("the rebuilt LSF advertises the signature", state->m17_signature_advertised, 1U);
    for (size_t i = 0U; i < 4U; i++) {
        expect_signature_slice(payload_bits, i * 16U);
        (void)m17_dispatch_stream_payload(opts, state, payload_bits, k_test_signature_fns[i], processed_bits);
    }
    err |= expect_u8("a frame played from the signature range leaves the signature incomplete",
                     state->m17_signature_verification_status, 5U);
    dsd_state_ext_free_all(state);
    return err;
}

/* A signed network stream never wraps (M17 spec 2.0.11, Digital Signatures): its frame numbers only rise. A delayed copy
   of voice frame 0 or 1 after the first signature frame (0x7FFC) is late, not four or five frames ahead: it plays
   nothing and does not take the stream back. */
static int
test_ip_signed_stream_frame_order_never_wraps(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t lsf_packed[M17_LSF_BYTES];
    uint8_t payloads[TEST_SIGNED_VOICE_FRAMES][M17_SIGNATURE_DIGEST_BYTES];
    int err = 0;
    ip_test_state(opts, state);
    build_checked_lsf(lsf_bits, lsf_packed, m17_compose_frame_info(1U, 2U, 0U, 0U, 3U, 1U, 0U), NULL);
    build_signed_payloads(payloads, TEST_SIGNED_VOICE_FRAMES);
    const uint16_t sid = 0x7B00U;
    for (int f = 0; f < TEST_SIGNED_VOICE_FRAMES; f++) {
        send_ip_stream_frame(opts, state, lsf_bits, sid, (uint16_t)f, 0U, payloads[f], 0);
    }
    send_ip_stream_frame(opts, state, lsf_bits, sid, (uint16_t)M17_STREAM_SIGNATURE_FN0, 0U, M17_REF_SIGNATURE_BYTES,
                         0);
    err |= expect_int("the first signature frame is taken", (int)state->m17_ip_stream_last_fn,
                      (int)M17_STREAM_SIGNATURE_FN0);
    for (uint16_t late = 0U; late < 2U; late++) {
        const int decodes = g_codec2_decode_calls;
        send_ip_stream_frame(opts, state, lsf_bits, sid, late, 0U, payloads[late], 0);
        err |=
            expect_int("a delayed voice frame after the signature plays nothing", g_codec2_decode_calls - decodes, 0);
        err |= expect_int("a delayed voice frame does not take the stream back", (int)state->m17_ip_stream_last_fn,
                          (int)M17_STREAM_SIGNATURE_FN0);
    }
    /* Signed once, signed for the stream: a datagram of it whose LSF names no signature neither wraps the counter back
       nor plays from the signature range. */
    uint8_t unsigned_bits[TEST_M17_LSF_BITS];
    build_checked_lsf(unsigned_bits, lsf_packed, m17_compose_frame_info(1U, 2U, 0U, 0U, 3U, 0U, 0U), NULL);
    int decodes = g_codec2_decode_calls;
    send_ip_stream_frame(opts, state, unsigned_bits, sid, 2U, 0U, payloads[2], 0);
    err |= expect_int("an unsigned datagram of a signed stream does not wrap it back",
                      (int)state->m17_ip_stream_last_fn, (int)M17_STREAM_SIGNATURE_FN0);
    send_ip_stream_frame(opts, state, unsigned_bits, sid, (uint16_t)M17_STREAM_SIGNATURE_FN1, 0U,
                         M17_REF_SIGNATURE_BYTES + 16U, 0);
    err |= expect_int("an unsigned datagram in a signed stream's signature range plays nothing",
                      g_codec2_decode_calls - decodes, 0);
    err |=
        expect_u8("the signed stream is not verified over the network", state->m17_signature_verification_status, 6U);
    for (size_t i = 2U; i < 4U; i++) {
        const uint16_t fn = k_test_signature_fns[i];
        send_ip_stream_frame(opts, state, lsf_bits, sid, (uint16_t)(fn & M17_STREAM_FRAME_COUNTER_MAX),
                             (uint8_t)((fn & M17_STREAM_FRAME_END_MASK) != 0U), M17_REF_SIGNATURE_BYTES + (i * 16U), 0);
    }
    err |= expect_int("the signed stream ends at its last chunk", (int)state->m17_ip_stream, DSD_M17_IP_STREAM_NONE);
    dsd_state_ext_free_all(state);
    return err;
}

/* While A plays, the first datagram heard of stream B is its final frame (not frame 0): A plays on, and B is remembered
   as ended, so an earlier frame of B arriving late does not open B over A. Only B's frame 0 starts B. */
static int
test_ip_foreign_eot_is_remembered(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t payload[M17_SIGNATURE_DIGEST_BYTES];
    int err = 0;
    ip_test_state(opts, state);
    build_voice_lsf(lsf_bits, 3U, 0);
    DSD_MEMSET(payload, 0x58, sizeof(payload));
    send_ip_stream_frame(opts, state, lsf_bits, 0xA100U, 0U, 0U, payload, 0);
    send_ip_stream_frame(opts, state, lsf_bits, 0xA100U, 1U, 0U, payload, 0);
    dsd_call_snapshot call;
    err |= expect_int("A's call", get_m17_call(state, &call), 1);
    const uint64_t a_epoch = call.epoch;
    send_ip_stream_frame(opts, state, lsf_bits, 0xB100U, 15U, 1U, payload, 0);
    const int decodes = g_codec2_decode_calls;
    send_ip_stream_frame(opts, state, lsf_bits, 0xB100U, 7U, 0U, payload, 0);
    err |= expect_int("B's late frame plays nothing", g_codec2_decode_calls - decodes, 0);
    err |= expect_int("A stays open", (int)state->m17_ip_stream_sid, 0xA100);
    err |= expect_int("A's call record", get_m17_call(state, &call), 1);
    err |= expect_int("A's call stays active", call.phase == DSD_CALL_PHASE_ACTIVE, 1);
    err |= expect_u64("A's call keeps its epoch", call.epoch, a_epoch);
    send_ip_stream_frame(opts, state, lsf_bits, 0xB100U, 0U, 0U, payload, 0);
    err |= expect_int("B's frame 0 starts B", (int)state->m17_ip_stream_sid, 0xB100);
    err |= expect_int("B's call record", get_m17_call(state, &call), 1);
    err |= expect_int("B is another call", call.epoch != a_epoch, 1);
    dsd_state_ext_free_all(state);
    return err;
}

/* A signed network stream is reported not verified over the network from its link setup, so an end with every
   signature datagram lost (EOTX, DISC, or another stream replacing it) never reads as the over-the-air "SIG INCOMPLETE".
 */
static int
test_ip_signed_stream_end_without_chunks(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t lsf_packed[M17_LSF_BYTES];
    uint8_t other_bits[TEST_M17_LSF_BITS];
    uint8_t payload[M17_SIGNATURE_DIGEST_BYTES];
    int err = 0;
    build_checked_lsf(lsf_bits, lsf_packed, m17_compose_frame_info(1U, 2U, 0U, 0U, 3U, 1U, 0U), NULL);
    build_voice_lsf(other_bits, 3U, 0);
    DSD_MEMSET(payload, 0x59, sizeof(payload));
    static const char* const k_ends[3] = {"EOTX", "DISC", "replacement"};
    for (int end = 0; end < 3; end++) {
        ip_test_state(opts, state);
        dsd_test_capture_stderr cap;
        char out[16384];
        if (dsd_test_capture_stderr_begin(&cap, "m17_sig_net") != 0) {
            return 1;
        }
        for (uint16_t fn = 0U; fn < 4U; fn++) {
            send_ip_stream_frame(opts, state, lsf_bits, 0x7C00U, fn, 0U, payload, 0);
        }
        const uint8_t status_open = state->m17_signature_verification_status;
        if (end < 2) {
            uint8_t control[10];
            build_ip_control(control, k_ends[end], M17_REF_LSF_SRC_CSD);
            m17_ip_dispatch_frame(opts, state, control, sizeof(control));
        } else {
            send_ip_stream_frame(opts, state, other_bits, 0x7C01U, 0U, 0U, payload, 0);
        }
        const uint8_t status_end = state->m17_signature_verification_status;
        (void)dsd_test_capture_stderr_end(&cap);
        (void)dsd_test_capture_stderr_read(&cap, out, sizeof(out));
        /* The end drops the verdict with everything else held from the stream (m17_confirm_reset()). */
        if (status_open != 6U || status_end != 0U || capture_has(out, sizeof(out), "SIG INCOMPLETE")
            || !capture_has(out, sizeof(out), "SIG NOT VERIFIED OVER NETWORK")) {
            DSD_FPRINTF(stderr, "signed IP stream ended by %s: status %u then %u, incomplete %d\n", k_ends[end],
                        (unsigned)status_open, (unsigned)status_end, capture_has(out, sizeof(out), "SIG INCOMPLETE"));
            err = 1;
        }
    }
    dsd_state_ext_free_all(state);
    return err;
}

/* The ended network streams are a fixed table (64 keys): past it the oldest is forgotten, and from then on (fail closed)
   a stream that is neither open nor remembered starts only at its frame 0, never joined at a later frame. Until then a
   stream is joined at any frame. */
static int
test_ip_ended_streams_are_bounded_and_fail_closed(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t payload[M17_SIGNATURE_DIGEST_BYTES];
    int err = 0;
    ip_test_state(opts, state);
    build_voice_lsf(lsf_bits, 3U, 0);
    DSD_MEMSET(payload, 0x5B, sizeof(payload));
    for (uint16_t sid = 0x0200U; sid < 0x0240U; sid++) {
        send_ip_stream_frame(opts, state, lsf_bits, sid, 0U, 1U, payload, 0);
    }
    send_ip_stream_frame(opts, state, lsf_bits, 0x0200U, 2U, 0U, payload, 0);
    err |= expect_int("64 ended streams are all remembered", (int)state->m17_ip_stream, DSD_M17_IP_STREAM_NONE);
    send_ip_stream_frame(opts, state, lsf_bits, 0x0300U, 5U, 0U, payload, 0);
    err |= expect_int("an unseen stream joins at a later frame", (int)state->m17_ip_stream, DSD_M17_IP_STREAM_OPEN);
    /* Its end is the 65th: the oldest (0x0200) is forgotten. */
    send_ip_stream_frame(opts, state, lsf_bits, 0x0300U, 6U, 1U, payload, 0);
    send_ip_stream_frame(opts, state, lsf_bits, 0x0200U, 3U, 0U, payload, 0);
    err |=
        expect_int("a forgotten stream's late frame opens nothing", (int)state->m17_ip_stream, DSD_M17_IP_STREAM_NONE);
    send_ip_stream_frame(opts, state, lsf_bits, 0x0201U, 3U, 0U, payload, 0);
    err |=
        expect_int("a remembered stream's late frame opens nothing", (int)state->m17_ip_stream, DSD_M17_IP_STREAM_NONE);
    const int decodes = g_codec2_decode_calls;
    send_ip_stream_frame(opts, state, lsf_bits, 0x0400U, 5U, 0U, payload, 0);
    err |= expect_int("past the table an unseen stream does not join late", (int)state->m17_ip_stream,
                      DSD_M17_IP_STREAM_NONE);
    err |= expect_int("past the table a late join plays nothing", g_codec2_decode_calls - decodes, 0);
    send_ip_stream_frame(opts, state, lsf_bits, 0x0400U, 0U, 0U, payload, 0);
    err |= expect_int("past the table frame 0 starts a stream", (int)state->m17_ip_stream, DSD_M17_IP_STREAM_OPEN);
    send_ip_stream_frame(opts, state, lsf_bits, 0x0400U, 1U, 0U, payload, 0);
    err |= expect_int("the started stream goes on", (int)state->m17_ip_stream_last_fn, 1);
    dsd_state_ext_free_all(state);
    return err;
}

/* The 1600 bit/s arbitrary data is assembled per six-frame group of one stream: a stream change between the group's
   frames empties it, so no group is made of two streams' chunks. */
static int
test_ip_1600_arbitrary_data_is_one_streams(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t lsf_packed[M17_LSF_BYTES];
    int err = 0;
    build_checked_lsf(lsf_bits, lsf_packed, m17_compose_frame_info(1U, 3U, 0U, 0U, 3U, 0U, 0U), NULL);
    for (int change = 0; change < 2; change++) {
        ip_test_state(opts, state);
        dsd_test_capture_stderr cap;
        char out[16384];
        if (dsd_test_capture_stderr_begin(&cap, "m17_adata") != 0) {
            return 1;
        }
        for (uint16_t fn = 0U; fn < 6U; fn++) {
            uint8_t payload[M17_SIGNATURE_DIGEST_BYTES];
            DSD_MEMSET(payload, 0, 8U);
            DSD_MEMSET(payload + 8, 'A' + fn, 8U);
            const uint16_t sid = (change != 0 && fn >= 3U) ? 0x7D01U : 0x7D00U;
            send_ip_stream_frame(opts, state, lsf_bits, sid, fn, 0U, payload, 0);
        }
        (void)dsd_test_capture_stderr_end(&cap);
        (void)dsd_test_capture_stderr_read(&cap, out, sizeof(out));
        /* The group's 48 bytes as one packet, shown in hex: slot 2's chunk runs into slot 3's, which no single frame's
           eight bytes do. */
        const int whole = capture_has(out, sizeof(out), "43434343434343434444444444444444");
        err |= expect_int(change ? "a group across two streams is never put together"
                                 : "one stream's group is put "
                                   "together",
                          whole, change ? 0 : 1);
    }
    dsd_state_ext_free_all(state);
    return err;
}

/* Text META is assembled per transmission: blocks of one stream's text never complete another's, though both texts
   are as long. A stream's end (here its EOT) empties what was assembled. */
static int
test_text_meta_is_one_transmissions(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t first_bits[TEST_M17_LSF_BITS];
    uint8_t second_bits[TEST_M17_LSF_BITS];
    uint8_t lsf_packed[M17_LSF_BYTES];
    uint8_t meta[M17_AES_NONCE_BYTES];
    uint8_t payload[M17_SIGNATURE_DIGEST_BYTES];
    int err = 0;
    const unsigned long long dst = m17_encode_b40_callsign(0ULL, M17_REF_LSF_DST_CSD);
    const unsigned long long src = m17_encode_b40_callsign(0ULL, M17_REF_LSF_SRC_CSD);
    /* Two-block texts: A sends its block 1 of 2, B its block 2 of 2. */
    DSD_MEMSET(meta, 0, sizeof(meta));
    meta[0] = 0x31U;
    DSD_MEMCPY(meta + 1, "FIRST-PART-AA", M17_TEXT_BLOCK_BYTES);
    build_lsf_bits(first_bits, dst, src, m17_compose_frame_info(1U, 2U, 0U, 0U, 3U, 0U, 0U), meta);
    (void)m17_attach_lsf_crc(first_bits, lsf_packed);
    DSD_MEMSET(meta, 0, sizeof(meta));
    meta[0] = 0x32U;
    DSD_MEMCPY(meta + 1, "OTHER-TEXT-BB", M17_TEXT_BLOCK_BYTES);
    build_lsf_bits(second_bits, dst, src, m17_compose_frame_info(1U, 2U, 0U, 0U, 3U, 0U, 0U), meta);
    (void)m17_attach_lsf_crc(second_bits, lsf_packed);
    DSD_MEMSET(payload, 0x5C, sizeof(payload));

    ip_test_state(opts, state);
    dsd_test_capture_stderr cap;
    char out[16384];
    if (dsd_test_capture_stderr_begin(&cap, "m17_text") != 0) {
        return 1;
    }
    send_ip_stream_frame(opts, state, first_bits, 0x7E00U, 0U, 0U, payload, 0);
    send_ip_stream_frame(opts, state, first_bits, 0x7E00U, 1U, 1U, payload, 0);
    send_ip_stream_frame(opts, state, second_bits, 0x7E01U, 0U, 0U, payload, 0);
    (void)dsd_test_capture_stderr_end(&cap);
    (void)dsd_test_capture_stderr_read(&cap, out, sizeof(out));
    err |=
        expect_int("two transmissions' text blocks never make one text", capture_has(out, sizeof(out), "Complete:"), 0);
    dsd_state_ext_free_all(state);
    return err;
}

/* Each packet is a whole transmission with its own LSF (M17 spec 2.0.11, Packet Mode), so its text META is its own:
   the blocks of two MPKTs, or of two packets over the air (the second's LSF frame failed its CRC, its EOT marker
   between was missed), never make one text, though both texts are as long. */
static int
test_packet_text_meta_is_one_packets(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t first[2U + M17_META_BYTES];
    uint8_t second[2U + M17_META_BYTES];
    uint8_t mpkt_frame[34U + sizeof(first) + 2U];
    uint8_t lsf_packed[M17_LSF_BYTES];
    int err = 0;
    const unsigned long long dst = m17_encode_b40_callsign(0ULL, M17_REF_LSF_DST_CSD);
    const unsigned long long src = m17_encode_b40_callsign(0ULL, M17_REF_LSF_SRC_CSD);
    build_lsf_bits(lsf_bits, dst, src, m17_compose_frame_info(0U, 0U, 0U, 0U, 5U, 0U, 0U), NULL);
    const uint16_t lsf_crc = m17_attach_lsf_crc(lsf_bits, lsf_packed);
    /* Two-block texts: the first packet carries block 1 of 2, the second block 2 of 2. */
    build_packet_text_meta_app(first, "FIRST-PART-AA");
    first[2] = 0x31U;
    build_packet_text_meta_app(second, "OTHER-TEXT-BB");
    second[2] = 0x32U;

    for (int rf = 0; rf < 2; rf++) {
        ip_test_state(opts, state);
        dsd_test_capture_stderr cap;
        char out[16384];
        if (dsd_test_capture_stderr_begin(&cap, "m17_pkt_text") != 0) {
            return 1;
        }
        const uint8_t* apps[2] = {first, second};
        for (int i = 0; i < 2; i++) {
            if (rf) {
                const uint16_t app_len = (uint16_t)sizeof(first);
                if (i == 1) {
                    DSD_MEMCPY(state->m17_lsf, lsf_bits, sizeof(lsf_bits));
                    (void)m17_finalize_lsf_crc(opts, state, lsf_packed, (uint16_t)(lsf_crc ^ 1U));
                }
                stage_packet_with_crc(state, apps[i], app_len, m17_crc16(apps[i], app_len));
                m17_pkt_finalize_eot(opts, state, app_len, (int)(app_len + M17_PACKET_CRC_BYTES));
            } else {
                const size_t len = build_ip_mpkt_frame(mpkt_frame, sizeof(mpkt_frame), lsf_bits,
                                                       (uint16_t)(0x7F00U + i), apps[i], sizeof(first));
                m17_ip_dispatch_frame(opts, state, mpkt_frame, (int)len);
            }
        }
        (void)dsd_test_capture_stderr_end(&cap);
        (void)dsd_test_capture_stderr_read(&cap, out, sizeof(out));
        err |= expect_int(rf ? "two RF packets' text blocks never make one text"
                             : "two MPKTs' text blocks never make one text",
                          capture_has(out, sizeof(out), "Complete:"), 0);
        if (rf) {
            /* The RF packet's text goes at the next transmission's first frame. */
            err |= expect_u8("an RF packet's end ends its transmission", state->m17_tx_ended, 1U);
            drop_ended_transmission(opts, state);
        }
        err |= expect_u8("a packet's text is gone after it", text_meta_received(state), 0U);
    }
    dsd_state_ext_free_all(state);
    return err;
}

/* Gate round 5 (issue #890): one boundary for everything a transmission held, and a text assembler per transmission. */

/* Under -F, A's LSF frame failed its CRC (its bits stay in the LSF being rebuilt), two clean chunks of A follow, the
   second with its EOS. With the EOT marker and B's LSF frame missed, B (encrypted, CAN 9) is acquired from its LICH:
   none of A's LSF bits or chunks may complete B's LSF, so B never plays as A's clear CAN 3. */
static int
test_rf_eos_drops_the_raw_lsf_bits(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t a_bits[TEST_M17_LSF_BITS];
    uint8_t a_packed[M17_LSF_BYTES];
    uint8_t b_bits[TEST_M17_LSF_BITS];
    uint8_t b_packed[M17_LSF_BYTES];
    uint8_t payload[M17_SIGNATURE_DIGEST_BYTES];
    uint8_t payload_bits[M17_STREAM_PAYLOAD_BITS];
    uint8_t processed_bits[M17_STREAM_PAYLOAD_BITS];
    uint16_t a_crc = 0U;
    int err = 0;
    ip_test_state(opts, state);
    opts->aggressive_framesync = 0;
    build_checked_lsf(a_bits, a_packed, m17_compose_frame_info(1U, 2U, 0U, 0U, 3U, 0U, 0U), &a_crc);
    (void)build_encrypted_lsf(b_bits, b_packed, 0U);
    DSD_MEMSET(payload, 0x6D, sizeof(payload));
    bytes_to_bits(payload, payload_bits, sizeof(payload));

    DSD_MEMCPY(state->m17_lsf, a_bits, sizeof(a_bits));
    err |= expect_int("A's LSF frame fails its CRC", m17_finalize_lsf_crc(opts, state, a_packed, a_crc ^ 1U), 1);
    err |= expect_u8("-F sets A up from it", state->m17_can, 3U);
    (void)feed_stream_lich(opts, state, a_bits, 0U);
    (void)feed_stream_lich(opts, state, a_bits, 1U);
    m17_stream_end_of_stream(opts, state);

    (void)feed_stream_lich(opts, state, b_bits, 5U);
    /* Its last-chunk rebuild held B's chunk alone: -F took nothing of A's setup from it. */
    err |= expect_u8("the rebuild at B's chunk 5 takes no stream type of A's", state->m17_str_dt,
                     (uint8_t)TEST_STREAM_TYPE_UNKNOWN);
    (void)feed_stream_lich(opts, state, b_bits, 0U);
    err |= expect_int("B is confirmed by its LICH", m17_confirm_is_confirmed(state), 1);
    err |= expect_int("B is not A's CAN 3", state->m17_can == 3U, 0);
    const int decodes = g_codec2_decode_calls;
    (void)m17_dispatch_stream_payload(opts, state, payload_bits, 6U, processed_bits);
    err |= expect_int("B never plays as A's clear voice", g_codec2_decode_calls - decodes, 0);
    dsd_state_ext_free_all(state);
    return err;
}

/* The LSF bits of a packet-mode LSF (CAN 5) carrying text META @p meta0 and @p text, CRC attached. */
static uint16_t
build_packet_text_lsf(uint8_t lsf_bits[TEST_M17_LSF_BITS], uint8_t lsf_packed[M17_LSF_BYTES], uint8_t meta0,
                      const char text[M17_TEXT_BLOCK_BYTES]) {
    uint8_t meta[M17_AES_NONCE_BYTES];
    DSD_MEMSET(meta, 0, sizeof(meta));
    meta[0] = meta0;
    DSD_MEMCPY(meta + 1, text, M17_TEXT_BLOCK_BYTES);
    const unsigned long long dst = m17_encode_b40_callsign(0ULL, M17_REF_LSF_DST_CSD);
    const unsigned long long src = m17_encode_b40_callsign(0ULL, M17_REF_LSF_SRC_CSD);
    build_lsf_bits(lsf_bits, dst, src, m17_compose_frame_info(0U, 0U, 0U, 0U, 5U, 0U, 0U), meta);
    return m17_attach_lsf_crc(lsf_bits, lsf_packed);
}

/* Packet A over the air: its checked LSF frame carries text block 1 of 2 and its packet fails its CRC. With A's EOT
   marker missed and B's LSF frame failing its CRC, B's checked packet carries block 2 of 2: the two never make one
   text. Then, under -F, a packet whose CRC failed is shown but neither adds its block nor clears the block its
   transmission's checked LSF gave. */
static int
test_rf_packet_text_is_its_transmissions(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t a_bits[TEST_M17_LSF_BITS];
    uint8_t a_packed[M17_LSF_BYTES];
    uint8_t sms[2U + M17_META_BYTES];
    uint8_t second[2U + M17_META_BYTES];
    int err = 0;
    const uint16_t a_crc = build_packet_text_lsf(a_bits, a_packed, 0x31U, "FIRST-PART-AA");
    DSD_MEMSET(sms, 0, sizeof(sms));
    sms[0] = 0x05U;
    DSD_MEMCPY(sms + 1, "HELLO", 5U);
    build_packet_text_meta_app(second, "OTHER-TEXT-BB");
    second[2] = 0x32U;
    const uint16_t app_len = (uint16_t)sizeof(second);

    for (int relaxed = 0; relaxed < 2; relaxed++) {
        ip_test_state(opts, state);
        opts->aggressive_framesync = relaxed ? 0 : 1;
        dsd_test_capture_stderr cap;
        char out[16384];
        if (dsd_test_capture_stderr_begin(&cap, "m17_rf_pkt_text") != 0) {
            return 1;
        }
        DSD_MEMCPY(state->m17_lsf, a_bits, sizeof(a_bits));
        (void)m17_finalize_lsf_crc(opts, state, a_packed, a_crc);
        const uint8_t after_lsf = text_meta_received(state);
        if (relaxed) {
            /* A's own packet fails its CRC: -F shows it, and its block joins nothing. */
            stage_packet_with_crc(state, second, app_len, (uint16_t)(m17_crc16(second, app_len) ^ 1U));
            m17_pkt_finalize_eot(opts, state, app_len, (int)(app_len + M17_PACKET_CRC_BYTES));
        } else {
            stage_packet_with_crc(state, sms, app_len, (uint16_t)(m17_crc16(sms, app_len) ^ 1U));
            m17_pkt_finalize_eot(opts, state, app_len, (int)(app_len + M17_PACKET_CRC_BYTES));
        }
        const uint8_t after_bad_packet = text_meta_received(state);
        if (!relaxed) {
            DSD_MEMCPY(state->m17_lsf, a_bits, sizeof(a_bits));
            (void)m17_finalize_lsf_crc(opts, state, a_packed, (uint16_t)(a_crc ^ 1U));
            stage_packet_with_crc(state, second, app_len, m17_crc16(second, app_len));
            m17_pkt_finalize_eot(opts, state, app_len, (int)(app_len + M17_PACKET_CRC_BYTES));
        }
        (void)dsd_test_capture_stderr_end(&cap);
        (void)dsd_test_capture_stderr_read(&cap, out, sizeof(out));
        err |= expect_u8("the checked LSF gives its block", after_lsf, 0x01U);
        err |= expect_u8(relaxed ? "a packet -F shows commits nothing and clears nothing"
                                 : "a packet whose CRC failed clears nothing",
                         after_bad_packet, 0x01U);
        err |= expect_int(relaxed ? "a packet -F shows completes no text" : "two packets' blocks never make one text",
                          capture_has(out, sizeof(out), "Complete:"), 0);
    }
    dsd_state_ext_free_all(state);
    return err;
}

/* An MPKT heard while a network stream is open is its own transmission: it neither adds to nor clears the open
   stream's text, whose blocks still make one text across it. */
static int
test_ip_mpkt_leaves_the_open_streams_text(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t first_bits[TEST_M17_LSF_BITS];
    uint8_t second_bits[TEST_M17_LSF_BITS];
    uint8_t packet_bits[TEST_M17_LSF_BITS];
    uint8_t lsf_packed[M17_LSF_BYTES];
    uint8_t meta[M17_AES_NONCE_BYTES];
    uint8_t payload[M17_SIGNATURE_DIGEST_BYTES];
    uint8_t sms[8];
    uint8_t mpkt_frame[34U + sizeof(sms) + 2U];
    int err = 0;
    const unsigned long long dst = m17_encode_b40_callsign(0ULL, M17_REF_LSF_DST_CSD);
    const unsigned long long src = m17_encode_b40_callsign(0ULL, M17_REF_LSF_SRC_CSD);
    DSD_MEMSET(meta, 0, sizeof(meta));
    meta[0] = 0x31U;
    DSD_MEMCPY(meta + 1, "STREAM-PART-1", M17_TEXT_BLOCK_BYTES);
    build_lsf_bits(first_bits, dst, src, m17_compose_frame_info(1U, 2U, 0U, 0U, 3U, 0U, 0U), meta);
    (void)m17_attach_lsf_crc(first_bits, lsf_packed);
    meta[0] = 0x32U;
    DSD_MEMCPY(meta + 1, "STREAM-PART-2", M17_TEXT_BLOCK_BYTES);
    build_lsf_bits(second_bits, dst, src, m17_compose_frame_info(1U, 2U, 0U, 0U, 3U, 0U, 0U), meta);
    (void)m17_attach_lsf_crc(second_bits, lsf_packed);
    build_lsf_bits(packet_bits, dst, src, m17_compose_frame_info(0U, 0U, 0U, 0U, 5U, 0U, 0U), NULL);
    DSD_MEMSET(sms, 0, sizeof(sms));
    sms[0] = 0x05U;
    DSD_MEMCPY(sms + 1, "HELLO", 5U);
    DSD_MEMSET(payload, 0x5D, sizeof(payload));

    ip_test_state(opts, state);
    dsd_test_capture_stderr cap;
    char out[16384];
    if (dsd_test_capture_stderr_begin(&cap, "m17_mpkt_stream_text") != 0) {
        return 1;
    }
    send_ip_stream_frame(opts, state, first_bits, 0x7F10U, 0U, 0U, payload, 0);
    const size_t len = build_ip_mpkt_frame(mpkt_frame, sizeof(mpkt_frame), packet_bits, 0x7F11U, sms, sizeof(sms));
    m17_ip_dispatch_frame(opts, state, mpkt_frame, (int)len);
    const uint8_t after_mpkt = text_meta_received(state);
    send_ip_stream_frame(opts, state, second_bits, 0x7F10U, 1U, 0U, payload, 0);
    (void)dsd_test_capture_stderr_end(&cap);
    (void)dsd_test_capture_stderr_read(&cap, out, sizeof(out));
    err |= expect_u8("an MPKT leaves the open stream's text", after_mpkt, 0x01U);
    err |= expect_int("the stream's blocks still make its text",
                      capture_has(out, sizeof(out), "Complete: STREAM-PART-1STREAM-PART-2"), 1);
    dsd_state_ext_free_all(state);
    return err;
}

/* A signed stream ends; its station starts an unsigned stream on the same stream ID at frame 0. A delayed signed copy
   of the old stream's frame 1 arriving after the new stream took its frame 1 is refused, and it does not make the new
   stream signed: its next frame plays, with no network signature note, and the stream keeps its counter's wrap. */
static int
test_ip_refused_signed_copy_signs_nothing(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t signed_bits[TEST_M17_LSF_BITS];
    uint8_t lsf_packed[M17_LSF_BYTES];
    uint8_t unsigned_bits[TEST_M17_LSF_BITS];
    uint8_t payload[M17_SIGNATURE_DIGEST_BYTES];
    int err = 0;
    build_checked_lsf(signed_bits, lsf_packed, m17_compose_frame_info(1U, 2U, 0U, 0U, 3U, 1U, 0U), NULL);
    build_voice_lsf(unsigned_bits, 3U, 0);
    DSD_MEMSET(payload, 0x5E, sizeof(payload));
    const uint16_t sid = 0x7F20U;

    ip_test_state(opts, state);
    for (uint16_t fn = 0U; fn < 4U; fn++) {
        send_ip_stream_frame(opts, state, signed_bits, sid, fn, (uint8_t)(fn == 3U), payload, 0);
    }
    send_ip_stream_frame(opts, state, unsigned_bits, sid, 0U, 0U, payload, 0);
    send_ip_stream_frame(opts, state, unsigned_bits, sid, 1U, 0U, payload, 0);
    send_ip_stream_frame(opts, state, signed_bits, sid, 1U, 0U, payload, 0);
    err |= expect_u8("a refused signed copy signs nothing", state->m17_ip_stream_signed, 0U);
    dsd_test_capture_stderr cap;
    char out[8192];
    if (dsd_test_capture_stderr_begin(&cap, "m17_ip_signed_copy") != 0) {
        return 1;
    }
    send_ip_stream_frame(opts, state, unsigned_bits, sid, 2U, 0U, payload, 0);
    (void)dsd_test_capture_stderr_end(&cap);
    (void)dsd_test_capture_stderr_read(&cap, out, sizeof(out));
    err |= expect_int("the unsigned stream's next frame is taken", (int)state->m17_ip_stream_last_fn, 2);
    err |= expect_int("it gets no network signature note",
                      capture_has(out, sizeof(out), "SIG NOT VERIFIED OVER NETWORK"), 0);
    err |= expect_u8("it has no signature verdict", state->m17_signature_verification_status, 0U);
    dsd_state_ext_free_all(state);
    return err;
}

#ifdef DSD_NEO_TEST_ALLOC_WRAP
/* Fail the next allocation the size of the M17 extension (its first creation), or every one while
   g_fail_m17_ext_allocs is set. */
static int g_fail_m17_ext_alloc;
static int g_fail_m17_ext_allocs;

// NOLINTNEXTLINE(bugprone-reserved-identifier,misc-use-internal-linkage)
void* __real_calloc(size_t count, size_t size);
// NOLINTNEXTLINE(bugprone-reserved-identifier,misc-use-internal-linkage)
void* __wrap_calloc(size_t count, size_t size);

void*
__wrap_calloc(size_t count, size_t size) {
    if (g_fail_m17_ext_alloc != 0 && count == 1U && size == sizeof(m17_state_ext)) {
        g_fail_m17_ext_alloc = 0;
        return NULL;
    }
    if (g_fail_m17_ext_allocs != 0 && count == 1U && size == sizeof(m17_state_ext)) {
        return NULL;
    }
    return __real_calloc(count, size);
}

/* The first network frame heard is stream A's final frame, and the table that would record A's end cannot be made:
   A's end is lost, so from then on a stream neither open nor remembered starts only at frame 0. An earlier frame of A
   arriving after stream B opened does not displace B. */
static int
test_ip_unrecordable_end_fails_closed(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t a_bits[TEST_M17_LSF_BITS];
    uint8_t b_bits[TEST_M17_LSF_BITS];
    uint8_t payload[M17_SIGNATURE_DIGEST_BYTES];
    int err = 0;
    build_voice_lsf(a_bits, 3U, 0);
    build_voice_lsf_from(b_bits, "N0OTHR");
    DSD_MEMSET(payload, 0x5F, sizeof(payload));

    ip_test_state(opts, state);
    g_fail_m17_ext_alloc = 1;
    send_ip_stream_frame(opts, state, a_bits, 0x7F30U, 9U, 1U, payload, 0);
    err |= expect_int("the allocation was tried and failed", g_fail_m17_ext_alloc, 0);
    send_ip_stream_frame(opts, state, b_bits, 0x7F31U, 0U, 0U, payload, 0);
    err |= expect_int("B opens at its frame 0", (int)state->m17_ip_stream_sid, 0x7F31);
    send_ip_stream_frame(opts, state, a_bits, 0x7F30U, 7U, 0U, payload, 0);
    err |= expect_int("A's earlier frame does not displace B", (int)state->m17_ip_stream_sid, 0x7F31);
    err |= expect_int("B stays open", (int)state->m17_ip_stream, DSD_M17_IP_STREAM_OPEN);
    dsd_state_ext_free_all(state);
    return err;
}

/* A failed allocation on an ordinary frame (no EOT) refuses that frame and loses no end: once the table can be made, the
   stream's next frame opens it, and streams joined part-way through are still taken. */
static int
test_ip_failed_allocation_on_an_ordinary_frame(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t payload[M17_SIGNATURE_DIGEST_BYTES];
    int err = 0;
    build_voice_lsf(lsf_bits, 3U, 0);
    DSD_MEMSET(payload, 0x60, sizeof(payload));

    ip_test_state(opts, state);
    g_fail_m17_ext_alloc = 1;
    send_ip_stream_frame(opts, state, lsf_bits, 0x7F40U, 5U, 0U, payload, 0);
    err |= expect_int("the allocation was tried and failed", g_fail_m17_ext_alloc, 0);
    err |= expect_int("the frame is refused", (int)state->m17_ip_stream, DSD_M17_IP_STREAM_NONE);
    err |= expect_u8("no end was lost", state->m17_ip_ended_lost, 0U);
    send_ip_stream_frame(opts, state, lsf_bits, 0x7F40U, 6U, 0U, payload, 0);
    err |= expect_int("its next frame opens the stream", (int)state->m17_ip_stream, DSD_M17_IP_STREAM_OPEN);
    err |= expect_int("the stream is the one joined", (int)state->m17_ip_stream_last_fn, 6);
    dsd_state_ext_free_all(state);
    return err;
}
#endif

/* An MPKT's payload is decoded against its own LSF, applied for it alone: at its end the decoded link setup is the one
   it found, whether no stream was open (nothing of the MPKT stays) or a stream it interleaves with keeps its own. An
   MPKT whose LSF is not applied (a reserved TYPE) inherits nothing: its payload is not decoded, even through a CAN
   filter an earlier MPKT's CAN would pass. */
static int
test_ip_mpkt_link_setup_is_its_own(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t can5_bits[TEST_M17_LSF_BITS];
    uint8_t reserved_bits[TEST_M17_LSF_BITS];
    uint8_t voice_bits[TEST_M17_LSF_BITS];
    uint8_t sms[8];
    uint8_t mpkt_frame[34U + sizeof(sms) + 2U];
    uint8_t payload[M17_SIGNATURE_DIGEST_BYTES];
    int err = 0;
    const unsigned long long dst = m17_encode_b40_callsign(0ULL, M17_REF_LSF_DST_CSD);
    const unsigned long long src = m17_encode_b40_callsign(0ULL, M17_REF_LSF_SRC_CSD);
    build_lsf_bits(can5_bits, dst, src, m17_compose_frame_info(0U, 0U, 0U, 0U, 5U, 0U, 0U), NULL);
    build_lsf_bits(reserved_bits, dst, src, m17_compose_frame_info(0U, 0U, 0U, 0U, 9U, 0U, 1U), NULL);
    build_voice_lsf(voice_bits, 3U, 0);
    DSD_MEMSET(sms, 0, sizeof(sms));
    sms[0] = 0x05U;
    DSD_MEMCPY(sms + 1, "HELLO", 5U);
    DSD_MEMSET(payload, 0x61, sizeof(payload));

    ip_test_state(opts, state);
    size_t len = build_ip_mpkt_frame(mpkt_frame, sizeof(mpkt_frame), can5_bits, 0x7F50U, sms, sizeof(sms));
    m17_ip_dispatch_frame(opts, state, mpkt_frame, (int)len);
    err |= expect_u8("a finished MPKT leaves no checked binding", state->m17_lsf_checked, 0U);
    err |= expect_u8("a finished MPKT leaves no CAN", state->m17_can, 0U);

    state->m17_can_en = 5;
    dsd_test_capture_stderr cap;
    char out[8192];
    if (dsd_test_capture_stderr_begin(&cap, "m17_mpkt_reserved") != 0) {
        return 1;
    }
    len = build_ip_mpkt_frame(mpkt_frame, sizeof(mpkt_frame), reserved_bits, 0x7F51U, sms, sizeof(sms));
    m17_ip_dispatch_frame(opts, state, mpkt_frame, (int)len);
    (void)dsd_test_capture_stderr_end(&cap);
    (void)dsd_test_capture_stderr_read(&cap, out, sizeof(out));
    err |= expect_int("an MPKT whose LSF is not applied decodes no payload", capture_has(out, sizeof(out), "HELLO"), 0);

    state->m17_can_en = -1;
    send_ip_stream_frame(opts, state, voice_bits, 0x7F52U, 0U, 0U, payload, 0);
    len = build_ip_mpkt_frame(mpkt_frame, sizeof(mpkt_frame), can5_bits, 0x7F53U, sms, sizeof(sms));
    m17_ip_dispatch_frame(opts, state, mpkt_frame, (int)len);
    err |= expect_u8("an interleaved MPKT leaves the stream's CAN", state->m17_can, 3U);
    err |= expect_u8("an interleaved MPKT leaves the stream's type", state->m17_str_dt, 2U);
    err |= expect_u8("an interleaved MPKT leaves the stream's checked binding", state->m17_lsf_checked, 1U);
    dsd_state_ext_free_all(state);
    return err;
}

/* An over-the-air packet whose earlier frames were lost ends at its valid last frame (EOF) all the same: nothing of it
   is decoded or checked, its call ends, and its transmission is over, so the next packet (its LSF frame failed its CRC,
   the EOT marker between missed) never completes the ended one's text. */
static void rf_frames_pass(dsd_state* state, unsigned frames);

static int
test_rf_packet_eof_without_its_prefix_ends_it(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t a_bits[TEST_M17_LSF_BITS];
    uint8_t a_packed[M17_LSF_BYTES];
    uint8_t second[2U + M17_META_BYTES];
    uint8_t eof_frame[26];
    int err = 0;
    const uint16_t a_crc = build_packet_text_lsf(a_bits, a_packed, 0x31U, "FIRST-PART-AA");
    build_packet_text_meta_app(second, "OTHER-TEXT-BB");
    second[2] = 0x32U;
    const uint16_t app_len = (uint16_t)sizeof(second);
    DSD_MEMSET(eof_frame, 0x42, sizeof(eof_frame));
    err |= expect_int("one-byte EOF metadata", m17_packet_metadata_byte(1U, 1U, &eof_frame[25]), 0);

    ip_test_state(opts, state);
    dsd_test_capture_stderr cap;
    char out[16384];
    if (dsd_test_capture_stderr_begin(&cap, "m17_rf_pkt_eof") != 0) {
        return 1;
    }
    DSD_MEMCPY(state->m17_lsf, a_bits, sizeof(a_bits));
    (void)m17_finalize_lsf_crc(opts, state, a_packed, a_crc);
    dsd_call_snapshot call;
    const int a_call = get_m17_call(state, &call) == 1 && call.phase == DSD_CALL_PHASE_ACTIVE;
    rf_frames_pass(state, 1U);
    const int used = m17_packet_take_chunk(opts, state, eof_frame);
    const int a_ended = get_m17_call(state, &call) == 1 && call.phase != DSD_CALL_PHASE_ACTIVE;
    DSD_MEMCPY(state->m17_lsf, a_bits, sizeof(a_bits));
    (void)m17_finalize_lsf_crc(opts, state, a_packed, (uint16_t)(a_crc ^ 1U));
    stage_packet_with_crc(state, second, app_len, m17_crc16(second, app_len));
    m17_pkt_finalize_eot(opts, state, app_len, (int)(app_len + M17_PACKET_CRC_BYTES));
    (void)dsd_test_capture_stderr_end(&cap);
    (void)dsd_test_capture_stderr_read(&cap, out, sizeof(out));
    err |= expect_int("A's checked LSF opened its call", a_call, 1);
    err |= expect_int("A's partial packet is not taken", used, 0);
    err |= expect_int("A's EOF ends its call", a_ended, 1);
    err |= expect_int("A's and B's blocks never make one text", capture_has(out, sizeof(out), "Complete:"), 0);
    dsd_state_ext_free_all(state);
    return err;
}

/* Gate round 7 (issue #890): an MPKT inside an open stream, and a packet that is not whole. */

/* An MPKT heard while a network stream is open is its own transmission and leaves the stream's call alone: it neither
   ends, splits nor replaces it, and the carrier's received identity stays the stream's. The stream's next frame
   continues the same call. */
static int
test_ip_mpkt_keeps_the_open_streams_call(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t voice_bits[TEST_M17_LSF_BITS];
    uint8_t packet_bits[TEST_M17_LSF_BITS];
    uint8_t sms[8];
    uint8_t mpkt_frame[34U + sizeof(sms) + 2U];
    uint8_t payload[M17_SIGNATURE_DIGEST_BYTES];
    int err = 0;
    const unsigned long long dst = m17_encode_b40_callsign(0ULL, M17_REF_LSF_DST_CSD);
    const unsigned long long src = m17_encode_b40_callsign(0ULL, M17_REF_LSF_SRC_CSD);
    build_voice_lsf(voice_bits, 3U, 0);
    build_lsf_bits(packet_bits, dst, src, m17_compose_frame_info(0U, 0U, 0U, 0U, 5U, 0U, 0U), NULL);
    DSD_MEMSET(sms, 0, sizeof(sms));
    sms[0] = 0x05U;
    DSD_MEMCPY(sms + 1, "HELLO", 5U);
    DSD_MEMSET(payload, 0x62, sizeof(payload));

    ip_test_state(opts, state);
    send_ip_stream_frame(opts, state, voice_bits, 0x7F60U, 0U, 0U, payload, 0);
    dsd_call_snapshot call;
    err |= expect_int("the stream's call", get_m17_call(state, &call), 1);
    const uint64_t epoch = call.epoch;
    const size_t len = build_ip_mpkt_frame(mpkt_frame, sizeof(mpkt_frame), packet_bits, 0x7F61U, sms, sizeof(sms));
    m17_ip_dispatch_frame(opts, state, mpkt_frame, (int)len);
    err |= expect_int("after the MPKT: call", get_m17_call(state, &call), 1);
    err |= expect_u64("the MPKT starts no epoch", call.epoch, epoch);
    err |= expect_int("the MPKT leaves the stream's call active", call.phase == DSD_CALL_PHASE_ACTIVE, 1);
    err |= expect_int("the stream's call stays voice", call.kind, DSD_CALL_KIND_VOICE);
    err |= expect_u8("the received CAN stays the stream's", (uint8_t)state->rx_id.access, 3U);
    send_ip_stream_frame(opts, state, voice_bits, 0x7F60U, 1U, 0U, payload, 0);
    err |= expect_int("next stream frame: call", get_m17_call(state, &call), 1);
    err |= expect_u64("the stream's next frame continues its call", call.epoch, epoch);
    dsd_state_ext_free_all(state);
    return err;
}

/* An MPKT is a packet (M17 spec 2.0.11, Packet Mode), whatever its LSF's Packet/Stream bit says: one whose LSF is
   stream-mode voice (taken for its META, which only a stream-mode TYPE can name) publishes a data call that ends with
   it, and never leaves a live voice call. */
static int
test_ip_mpkt_with_a_stream_mode_lsf_leaves_no_voice_call(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t stream_bits[TEST_M17_LSF_BITS];
    uint8_t sms[8];
    uint8_t mpkt_frame[34U + sizeof(sms) + 2U];
    int err = 0;
    build_voice_lsf(stream_bits, 3U, 0);
    DSD_MEMSET(sms, 0, sizeof(sms));
    sms[0] = 0x05U;
    DSD_MEMCPY(sms + 1, "HELLO", 5U);

    ip_test_state(opts, state);
    const size_t len = build_ip_mpkt_frame(mpkt_frame, sizeof(mpkt_frame), stream_bits, 0x7F70U, sms, sizeof(sms));
    m17_ip_dispatch_frame(opts, state, mpkt_frame, (int)len);
    dsd_call_snapshot call;
    err |= expect_int("a stream-mode MPKT published its call", get_m17_call(state, &call), 1);
    err |= expect_int("a stream-mode MPKT's call is a data call", call.kind, DSD_CALL_KIND_DATA);
    err |= expect_int("a stream-mode MPKT leaves no active call", call.phase == DSD_CALL_PHASE_ACTIVE, 0);
    dsd_state_ext_free_all(state);
    return err;
}

/* The over-the-air packet frame of chunk @p counter (@p eof: its last, @p counter then its byte count) whose first
   bytes are @p bytes. */
static void
build_packet_chunk(uint8_t frame[26], uint8_t eof, uint8_t counter, const uint8_t* bytes, size_t n) {
    DSD_MEMSET(frame, 0, 26U);
    DSD_MEMCPY(frame, bytes, n);
    (void)m17_packet_metadata_byte(eof, counter, &frame[25]);
}

/* @p frames M17 frames (192 symbols each) read since the last: where dsd_state::symbolcnt stands when the next frame
   ends. */
static void
rf_frames_pass(dsd_state* state, unsigned frames) {
    state->symbolcnt += frames * 192U;
}

/* A packet-mode LSF frame (CAN 5, no META), its CRC passing or (@p corrupt) failing, ending where dsd_state::symbolcnt
   stands. */
static void
rf_packet_lsf_frame(dsd_opts* opts, dsd_state* state, int corrupt) {
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t lsf_packed[M17_LSF_BYTES];
    const unsigned long long dst = m17_encode_b40_callsign(0ULL, M17_REF_LSF_DST_CSD);
    const unsigned long long src = m17_encode_b40_callsign(0ULL, M17_REF_LSF_SRC_CSD);
    build_lsf_bits(lsf_bits, dst, src, m17_compose_frame_info(0U, 0U, 0U, 0U, 5U, 0U, 0U), NULL);
    const uint16_t crc = m17_attach_lsf_crc(lsf_bits, lsf_packed);
    DSD_MEMCPY(state->m17_lsf, lsf_bits, sizeof(lsf_bits));
    (void)m17_finalize_lsf_crc(opts, state, lsf_packed, corrupt ? (uint16_t)(crc ^ 1U) : crc);
}

/* An SMS packet of @p text (its CRC attached) in @p out; its length. */
static size_t
build_sms_packet(uint8_t* out, size_t cap, const char* text) {
    const size_t n = strlen(text);
    if (n + 3U > cap) {
        return 0U;
    }
    out[0] = 0x05U;
    DSD_MEMCPY(out + 1, text, n);
    const uint16_t crc = m17_crc16(out, (uint16_t)(n + 1U));
    out[n + 1U] = (uint8_t)(crc >> 8U);
    out[n + 2U] = (uint8_t)(crc & 0xFFU);
    return n + 3U;
}

/* Packet frame @p index of the packet @p pkt of @p len bytes: a 25-byte chunk, or the last one (EOF) with its byte
   count. */
static void
build_sms_frame(uint8_t frame[26], const uint8_t* pkt, size_t len, size_t index) {
    const size_t at = index * 25U;
    const size_t left = len - at;
    if (left > 25U) {
        build_packet_chunk(frame, 0U, (uint8_t)index, pkt + at, 25U);
    } else {
        build_packet_chunk(frame, 1U, (uint8_t)left, pkt + at, left);
    }
}

/* Feed packet frames @p frames (indexes into the packet, -1 for a frame lost on the air) one M17 frame apart after an
   LSF frame (@p lsf_corrupt: its CRC failed), under -F; what was printed. Returns the last frame's verdict. */
static int
rf_sms_frames(dsd_opts* opts, dsd_state* state, const uint8_t* pkt, size_t len, const int* frames, size_t n, char* out,
              size_t cap) {
    DSD_MEMSET(out, 0, cap); /* nothing printed, should the capture not start */
    dsd_test_capture_stderr cap_err;
    if (dsd_test_capture_stderr_begin(&cap_err, "m17_rf_sms") != 0) {
        return -1;
    }
    rf_packet_lsf_frame(opts, state, 0);
    int verdict = 0;
    for (size_t i = 0U; i < n; i++) {
        rf_frames_pass(state, 1U);
        if (frames[i] < 0) {
            continue;
        }
        uint8_t frame[26];
        build_sms_frame(frame, pkt, len, (size_t)frames[i]);
        verdict = m17_packet_take_chunk(opts, state, frame);
    }
    (void)dsd_test_capture_stderr_end(&cap_err);
    (void)dsd_test_capture_stderr_read(&cap_err, out, cap);
    return verdict;
}

/* A packet that is not whole is never decoded or credited at its last frame (EOF), under -F too:
   - one whose frame before its EOF was lost on the air (its EOF carries a byte count, not a frame number, so only the
     symbol spacing from the frame before tells; M17 2.0.11, Packet Frames), whether the frame lost was the first after
     the LSF or a later one;
   - one whose chunk 0 was lost, though a later chunk's counter is misread as 0: a known gap is cleared only by a new
     transmission, never by a counter;
   - one with no application byte (an EOF of two bytes FF FF, the CRC of nothing; M17 Packet Data begins with its
     protocol byte): no confirmation and no consumption credit.
   Each EOF still ends its transmission. A whole packet decodes. */
static int
test_rf_packet_not_whole_is_not_decoded(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t pkt[96];
    char out[8192];
    int err = 0;
    const size_t len = build_sms_packet(pkt, sizeof(pkt), "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopq");

    ip_test_state(opts, state);
    opts->aggressive_framesync = 0;
    static const int k_whole[3] = {0, 1, 2};
    err |= expect_int("a whole packet's EOF is taken",
                      rf_sms_frames(opts, state, pkt, len, k_whole, 3U, out, sizeof(out)), 1);
    err |= expect_int("a whole packet decodes", capture_has(out, sizeof(out), "SMS: ABCDEFGHIJ"), 1);

    static const int k_middle_lost[3] = {0, -1, 2};
    ip_test_state(opts, state);
    opts->aggressive_framesync = 0;
    err |= expect_int("an EOF after a lost frame is not taken",
                      rf_sms_frames(opts, state, pkt, len, k_middle_lost, 3U, out, sizeof(out)), 0);
    err |=
        expect_int("a packet missing its middle frame is not decoded", capture_has(out, sizeof(out), "Protocol:"), 0);
    err |= expect_u8("its EOF still ends the transmission", state->m17_tx_ended, 1U);

    uint8_t two[40];
    const size_t two_len = build_sms_packet(two, sizeof(two), "ABCDEFGHIJKLMNOPQRSTUVWXYZ012");
    static const int k_first_lost[2] = {-1, 1};
    ip_test_state(opts, state);
    opts->aggressive_framesync = 0;
    (void)rf_sms_frames(opts, state, two, two_len, k_first_lost, 2U, out, sizeof(out));
    err |= expect_int("a packet missing the first frame after its LSF is not decoded",
                      capture_has(out, sizeof(out), "Protocol:"), 0);

    /* Chunk 0 lost, chunk 1 heard (a gap), chunk 2's counter misread as 0, then the EOF. */
    ip_test_state(opts, state);
    opts->aggressive_framesync = 0;
    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, "m17_pkt_gap") != 0) {
        return 1;
    }
    rf_packet_lsf_frame(opts, state, 0);
    uint8_t frame[26];
    rf_frames_pass(state, 2U);
    build_sms_frame(frame, pkt, len, 1U);
    (void)m17_packet_take_chunk(opts, state, frame);
    rf_frames_pass(state, 1U);
    build_packet_chunk(frame, 0U, 0U, pkt + 50, 25U);
    (void)m17_packet_take_chunk(opts, state, frame);
    rf_frames_pass(state, 1U);
    build_sms_frame(frame, pkt, len, 2U);
    (void)m17_packet_take_chunk(opts, state, frame);
    (void)dsd_test_capture_stderr_end(&cap);
    (void)dsd_test_capture_stderr_read(&cap, out, sizeof(out));
    err |= expect_int("a counter read as 0 clears no known gap", capture_has(out, sizeof(out), "Protocol:"), 0);

    /* Chunk 0 lost (chunk 1 ends two frames after the LSF frame) and chunk 1's counter misread as 0: a measured loss is
       a loss whatever the counter says, so the EOF after it decodes nothing. */
    ip_test_state(opts, state);
    opts->aggressive_framesync = 0;
    if (dsd_test_capture_stderr_begin(&cap, "m17_pkt_zero_after_gap") != 0) {
        return 1;
    }
    rf_packet_lsf_frame(opts, state, 0);
    rf_frames_pass(state, 2U);
    build_packet_chunk(frame, 0U, 0U, pkt + 25, 25U);
    (void)m17_packet_take_chunk(opts, state, frame);
    rf_frames_pass(state, 1U);
    build_sms_frame(frame, pkt, len, 2U);
    (void)m17_packet_take_chunk(opts, state, frame);
    (void)dsd_test_capture_stderr_end(&cap);
    (void)dsd_test_capture_stderr_read(&cap, out, sizeof(out));
    err |= expect_int("a zero counter after a measured loss decodes nothing",
                      capture_has(out, sizeof(out), "Protocol:"), 0);

    /* Packet A's chunk 0, its end lost, then packet B's LSF frame with its CRC failed and B's one-frame EOF one frame
       later: a failed LSF frame starts nothing and drops A's bytes, so the two never join (the strict RF packet rule). */
    uint8_t one[16];
    const size_t one_len = build_sms_packet(one, sizeof(one), "BBBB");
    ip_test_state(opts, state);
    opts->aggressive_framesync = 0;
    if (dsd_test_capture_stderr_begin(&cap, "m17_pkt_join") != 0) {
        return 1;
    }
    rf_packet_lsf_frame(opts, state, 0);
    rf_frames_pass(state, 1U);
    build_sms_frame(frame, pkt, len, 0U);
    (void)m17_packet_take_chunk(opts, state, frame);
    rf_frames_pass(state, 1U);
    rf_packet_lsf_frame(opts, state, 1);
    rf_frames_pass(state, 1U);
    build_sms_frame(frame, one, one_len, 0U);
    (void)m17_packet_take_chunk(opts, state, frame);
    (void)dsd_test_capture_stderr_end(&cap);
    (void)dsd_test_capture_stderr_read(&cap, out, sizeof(out));
    err |= expect_int("a failed LSF frame joins no two packets", capture_has(out, sizeof(out), "Protocol:"), 0);

    /* An empty packet after its checked LSF frame: no consumption credit for its EOF, the LSF's confirmation kept. */
    ip_test_state(opts, state);
    rf_packet_lsf_frame(opts, state, 0);
    rf_frames_pass(state, 1U);
    {
        static const uint8_t k_nothing[2] = {0xFFU, 0xFFU};
        build_packet_chunk(frame, 1U, 2U, k_nothing, sizeof(k_nothing));
        err |= expect_int("an empty packet after its checked LSF gets no credit",
                          m17_packet_take_chunk(opts, state, frame), 0);
        err |= expect_u8("its EOF ends the transmission", state->m17_tx_ended, 1U);
        err |= expect_int("the checked LSF's confirmation stands", m17_confirm_is_confirmed(state), 1);
    }

    /* An empty packet after an LSF frame whose CRC failed: an unconfirmed chain the EOF must not credit. */
    ip_test_state(opts, state);
    rf_packet_lsf_frame(opts, state, 1);
    rf_frames_pass(state, 1U);
    static const uint8_t k_empty[2] = {0xFFU, 0xFFU};
    build_packet_chunk(frame, 1U, 2U, k_empty, sizeof(k_empty));
    err |= expect_int("an empty packet's EOF gets no consumption credit", m17_packet_take_chunk(opts, state, frame), 0);
    err |= expect_int("an empty packet earns no confirmation", m17_confirm_is_confirmed(state), 0);
    err |= expect_u8("an empty packet's EOF ends the transmission", state->m17_tx_ended, 1U);
    dsd_state_ext_free_all(state);
    return err;
}

/* Needs no linker wrap: it ends the call and drops the stream itself. */
/* A data stream on the network a lost carrier ended (noCarrier(), as sim_nocar runs it: a recoverable end, and every
   M17 boundary dropped); an MPKT then heard at once with the same source, destination and CAN, within the end's
   reacquisition window, is a transmission of its own: its call is a new epoch and a new transmission, not the stream's
   reopened. */
static int
test_ip_mpkt_after_a_faded_stream_is_its_own_transmission(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t data_bits[TEST_M17_LSF_BITS];
    uint8_t packet_bits[TEST_M17_LSF_BITS];
    uint8_t sms[8];
    uint8_t mpkt_frame[34U + sizeof(sms) + 2U];
    uint8_t payload[M17_SIGNATURE_DIGEST_BYTES];
    int err = 0;
    const unsigned long long dst = m17_encode_b40_callsign(0ULL, M17_REF_LSF_DST_CSD);
    const unsigned long long src = m17_encode_b40_callsign(0ULL, M17_REF_LSF_SRC_CSD);
    build_lsf_bits(data_bits, dst, src, m17_compose_frame_info(1U, 1U, 0U, 0U, 5U, 0U, 0U), NULL);
    build_lsf_bits(packet_bits, dst, src, m17_compose_frame_info(0U, 0U, 0U, 0U, 5U, 0U, 0U), NULL);
    DSD_MEMSET(sms, 0, sizeof(sms));
    sms[0] = 0x05U;
    DSD_MEMCPY(sms + 1, "HELLO", 5U);
    DSD_MEMSET(payload, 0x65, sizeof(payload));

    ip_test_state(opts, state);
    send_ip_stream_frame(opts, state, data_bits, 0x7FA0U, 0U, 0U, payload, 0);
    dsd_call_snapshot call;
    err |= expect_int("the data stream's call", get_m17_call(state, &call), 1);
    const uint64_t stream_epoch = call.epoch;
    const uint64_t stream_tx = dsd_call_snapshot_transmission(&call);
    err |= expect_int("the fade ends the stream's call",
                      dsd_call_state_end_ex(state, 0U, 0.0, DSD_CALL_END_SYNC_LOSS) > 0, 1);
    m17_confirm_reset(state);
    const size_t len = build_ip_mpkt_frame(mpkt_frame, sizeof(mpkt_frame), packet_bits, 0x7FA1U, sms, sizeof(sms));
    m17_ip_dispatch_frame(opts, state, mpkt_frame, (int)len);
    err |= expect_int("after the MPKT: call", get_m17_call(state, &call), 1);
    err |= expect_int("the MPKT's call is a new epoch", call.epoch != stream_epoch, 1);
    err |= expect_int("the MPKT's call is a transmission of its own",
                      dsd_call_snapshot_transmission(&call) != stream_tx, 1);
    err |= expect_int("its call is a data call", call.kind, DSD_CALL_KIND_DATA);
    dsd_state_ext_free_all(state);
    return err;
}

#ifdef DSD_NEO_TEST_ALLOC_WRAP
/* The real-time monotonic clock the M17 network idle rule reads, held where a test puts it (0: the real clock). */
static uint64_t g_fake_realtime_ms;

// NOLINTNEXTLINE(bugprone-reserved-identifier,misc-use-internal-linkage)
uint64_t __real_dsd_realtime_mono_ms(void);
// NOLINTNEXTLINE(bugprone-reserved-identifier,misc-use-internal-linkage)
uint64_t __wrap_dsd_realtime_mono_ms(void);

uint64_t
__wrap_dsd_realtime_mono_ms(void) {
    return g_fake_realtime_ms != 0U ? g_fake_realtime_ms : __real_dsd_realtime_mono_ms();
}

/* The M17 extension cannot be made when a packet's checked LSF frame requests it (once, for its text and the packet
   it starts): no packet is assembled behind it, also when a second request in the same frame would succeed, and once
   memory is back its chunk 0 (no counter or allocation alone resumes an assembly) and its EOF still decode nothing
   (the strict RF packet rule). */
static int
test_rf_packet_unarmed_without_memory(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t pkt[48];
    uint8_t frame[26];
    char out[8192];
    int err = 0;
    const size_t len = build_sms_packet(pkt, sizeof(pkt), "ABCDEFGHIJKLMNOPQRSTUVWXYZ012");

    /* The LSF frame's one request fails (a retry within the same frame would succeed), then every request during it. */
    for (int every = 0; every <= 1; every++) {
        ip_test_state(opts, state);
        opts->aggressive_framesync = 0;
        dsd_test_capture_stderr cap;
        if (dsd_test_capture_stderr_begin(&cap, "m17_pkt_no_memory") != 0) {
            return 1;
        }
        g_fail_m17_ext_alloc = 1;
        g_fail_m17_ext_allocs = every;
        rf_packet_lsf_frame(opts, state, 0);
        const int tried = g_fail_m17_ext_alloc == 0;
        g_fail_m17_ext_alloc = 0;
        g_fail_m17_ext_allocs = 0;
        rf_frames_pass(state, 1U);
        build_sms_frame(frame, pkt, len, 0U);
        (void)m17_packet_take_chunk(opts, state, frame);
        rf_frames_pass(state, 1U);
        build_sms_frame(frame, pkt, len, 1U);
        (void)m17_packet_take_chunk(opts, state, frame);
        (void)dsd_test_capture_stderr_end(&cap);
        DSD_MEMSET(out, 0, sizeof(out));
        (void)dsd_test_capture_stderr_read(&cap, out, sizeof(out));
        err |= expect_int("the LSF frame's allocation was tried and failed", tried, 1);
        err |=
            expect_int("a packet its LSF could not arm is not decoded", capture_has(out, sizeof(out), "Protocol:"), 0);
        dsd_state_ext_free_all(state);
    }
    return err;
}

/* A data stream on the network whose end was lost goes idle; an MPKT then heard with the same source, destination and
   CAN is a transmission of its own: it starts its own call, not a continuation of the stream's. */
static int
test_ip_mpkt_after_an_idle_like_stream_starts_its_own_call(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t data_bits[TEST_M17_LSF_BITS];
    uint8_t packet_bits[TEST_M17_LSF_BITS];
    uint8_t sms[8];
    uint8_t mpkt_frame[34U + sizeof(sms) + 2U];
    uint8_t payload[M17_SIGNATURE_DIGEST_BYTES];
    int err = 0;
    const unsigned long long dst = m17_encode_b40_callsign(0ULL, M17_REF_LSF_DST_CSD);
    const unsigned long long src = m17_encode_b40_callsign(0ULL, M17_REF_LSF_SRC_CSD);
    build_lsf_bits(data_bits, dst, src, m17_compose_frame_info(1U, 1U, 0U, 0U, 5U, 0U, 0U), NULL);
    build_lsf_bits(packet_bits, dst, src, m17_compose_frame_info(0U, 0U, 0U, 0U, 5U, 0U, 0U), NULL);
    DSD_MEMSET(sms, 0, sizeof(sms));
    sms[0] = 0x05U;
    DSD_MEMCPY(sms + 1, "HELLO", 5U);
    DSD_MEMSET(payload, 0x64, sizeof(payload));

    ip_test_state(opts, state);
    g_fake_realtime_ms = 2000000U;
    send_ip_stream_frame(opts, state, data_bits, 0x7F90U, 0U, 0U, payload, 0);
    dsd_call_snapshot call;
    err |= expect_int("the data stream's call", get_m17_call(state, &call), 1);
    const uint64_t epoch = call.epoch;
    g_fake_realtime_ms += 8000U;
    const size_t len = build_ip_mpkt_frame(mpkt_frame, sizeof(mpkt_frame), packet_bits, 0x7F91U, sms, sizeof(sms));
    m17_ip_dispatch_frame(opts, state, mpkt_frame, (int)len);
    err |= expect_int("after the MPKT: call", get_m17_call(state, &call), 1);
    err |= expect_int("the MPKT starts its own call", call.epoch != epoch, 1);
    err |=
        expect_int("the idle stream stays open for its own frames", (int)state->m17_ip_stream, DSD_M17_IP_STREAM_OPEN);
    g_fake_realtime_ms = 0U;
    dsd_state_ext_free_all(state);
    return err;
}

/* A network stream whose final frame was lost, with no EOTX or DISC after it, stays open, but once no frame of it has
   been taken for longer than the idle limit it no longer holds packets back: an MPKT then publishes its own data call
   and received CAN. Within the limit the stream's call is kept (test_ip_mpkt_keeps_the_open_streams_call()). */
static int
test_ip_idle_stream_does_not_hold_back_packets(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t voice_bits[TEST_M17_LSF_BITS];
    uint8_t packet_bits[TEST_M17_LSF_BITS];
    uint8_t sms[8];
    uint8_t mpkt_frame[34U + sizeof(sms) + 2U];
    uint8_t payload[M17_SIGNATURE_DIGEST_BYTES];
    int err = 0;
    const unsigned long long dst = m17_encode_b40_callsign(0ULL, M17_REF_LSF_DST_CSD);
    const unsigned long long src = m17_encode_b40_callsign(0ULL, M17_REF_LSF_SRC_CSD);
    build_voice_lsf(voice_bits, 3U, 0);
    build_lsf_bits(packet_bits, dst, src, m17_compose_frame_info(0U, 0U, 0U, 0U, 5U, 0U, 0U), NULL);
    DSD_MEMSET(sms, 0, sizeof(sms));
    sms[0] = 0x05U;
    DSD_MEMCPY(sms + 1, "HELLO", 5U);
    DSD_MEMSET(payload, 0x63, sizeof(payload));

    ip_test_state(opts, state);
    g_fake_realtime_ms = 1000000U;
    send_ip_stream_frame(opts, state, voice_bits, 0x7F80U, 0U, 0U, payload, 0);
    dsd_call_snapshot call;
    err |= expect_int("the stream's call", get_m17_call(state, &call), 1);
    const uint64_t epoch = call.epoch;
    g_fake_realtime_ms += 1000U;
    size_t len = build_ip_mpkt_frame(mpkt_frame, sizeof(mpkt_frame), packet_bits, 0x7F81U, sms, sizeof(sms));
    m17_ip_dispatch_frame(opts, state, mpkt_frame, (int)len);
    err |= expect_int("within the idle limit: call", get_m17_call(state, &call), 1);
    err |= expect_u64("within the idle limit the MPKT keeps the stream's call", call.epoch, epoch);
    g_fake_realtime_ms += 3000U;
    len = build_ip_mpkt_frame(mpkt_frame, sizeof(mpkt_frame), packet_bits, 0x7F82U, sms, sizeof(sms));
    m17_ip_dispatch_frame(opts, state, mpkt_frame, (int)len);
    err |= expect_int("past the idle limit: call", get_m17_call(state, &call), 1);
    err |= expect_int("past the idle limit the MPKT publishes its own call", call.epoch != epoch, 1);
    err |= expect_int("its call is a data call", call.kind, DSD_CALL_KIND_DATA);
    err |= expect_u8("its CAN is the received one", (uint8_t)state->rx_id.access, 5U);
    g_fake_realtime_ms = 0U;
    dsd_state_ext_free_all(state);
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
    err |= test_lsf_application_stores_state_and_keeps_signature();
    err |= test_rebuilt_lsf_is_strong_evidence();
    err |= test_relaxed_lsf_keeps_checked_fields();
    err |= test_signed_rf_stream_verifies_across_lich_rebuilds();
    err |= test_signed_ip_stream_is_not_verified();
    err |= test_m17_end_ends_only_an_m17_call();
    err |= test_ip_late_copy_after_eot_is_inert();
    err |= test_ip_stream_id_reused_after_one_frame_call();
    err |= test_ip_late_copy_after_joining_on_signature_frames();
    err |= test_ip_checked_and_crc_failed_voice();
    err |= test_bad_repeated_lsf_keeps_signature_advertised();
    err |= test_m17_end_finalizes_a_sync_loss_end();
    err |= test_ip_duplicate_frames_are_dropped();
    err |= test_ip_unusable_link_setup_dispatches_nothing();
    err |= test_ip_end_controls_match_the_station();
    err |= test_ip_ended_stream_is_ignored_until_frame_0();
    err |= test_signed_rf_stream_needs_every_frame_once();
    err |= test_rf_marker_end_reports_an_incomplete_signature();
    err |= test_rf_eos_retires_the_signature();
    err |= test_ip_foreign_eot_does_not_end_the_open_stream();
    err |= test_ip_frame_counter_rollover();
    err |= test_ip_in_band_end_drops_the_carrier();
    err |= test_ip_replaced_stream_ends_its_call();
    err |= test_ip_ended_streams_by_station();
    err |= test_ip_packet_mode_lsf_in_a_stream_datagram();
    err |= test_ip_decoded_speech_marks_media();
    err |= test_relaxed_lsf_logs_its_own_iv();
    err |= test_rf_eos_retires_the_link_setup();
    err |= test_rf_eos_retires_the_decoded_link_setup();
    err |= test_link_setup_boundaries_clear_the_same_set();
    err |= test_signed_rf_signature_range_frames();
    err |= test_ip_signed_stream_frame_order_never_wraps();
    err |= test_ip_foreign_eot_is_remembered();
    err |= test_ip_signed_stream_end_without_chunks();
    err |= test_ip_ended_streams_are_bounded_and_fail_closed();
    err |= test_ip_1600_arbitrary_data_is_one_streams();
    err |= test_text_meta_is_one_transmissions();
    err |= test_packet_text_meta_is_one_packets();
    err |= test_rf_eos_drops_the_raw_lsf_bits();
    err |= test_rf_packet_text_is_its_transmissions();
    err |= test_ip_mpkt_leaves_the_open_streams_text();
    err |= test_ip_refused_signed_copy_signs_nothing();
#ifdef DSD_NEO_TEST_ALLOC_WRAP
    err |= test_ip_unrecordable_end_fails_closed();
    err |= test_ip_failed_allocation_on_an_ordinary_frame();
#endif
    err |= test_ip_mpkt_link_setup_is_its_own();
    err |= test_rf_packet_eof_without_its_prefix_ends_it();
    err |= test_ip_mpkt_keeps_the_open_streams_call();
    err |= test_ip_mpkt_with_a_stream_mode_lsf_leaves_no_voice_call();
    err |= test_rf_packet_not_whole_is_not_decoded();
#ifdef DSD_NEO_TEST_ALLOC_WRAP
    err |= test_ip_idle_stream_does_not_hold_back_packets();
    err |= test_rf_packet_unarmed_without_memory();
    err |= test_ip_mpkt_after_an_idle_like_stream_starts_its_own_call();
#endif
    err |= test_ip_mpkt_after_a_faded_stream_is_its_own_transmission();
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
    err |= test_lsf_names_stream_mode_only_behind_its_crc();
    err |= test_lsf_search_evidence_follows_the_published_call();
    err |= test_unconfirmed_stream_publishes_no_call();
    err |= test_stream_voice_chain_covers_the_whole_frame();
    err |= test_stream_voice_chain_onepoles_run_at_8k();
    err |= test_encoder_pcm_input_is_anti_aliased();
#ifdef USE_RADIO
    err |= test_encoder_rtl_input_reaches_pcm_scale();
    err |= test_m17_encoder_gap_drops_partial_block();
    err |= test_m17_encoder_survives_yield();
#endif
#ifdef USE_CODEC2
    err |= test_stream_voice_3200_dispatch_routes_pair_audio_to_udp();
    err |= test_stream_voice_1600_dispatch_routes_single_audio_to_udp();
    err |= test_stream_voice_audio_gate_suppresses_udp_when_slot_disabled();
    err |= test_stream_voice_audio_honors_mute_on_every_output();
    err |= test_stream_voice_audio_stamps_what_it_plays();
    err |= test_stream_voice_audio_stamps_nothing_no_output_takes();
    err |= test_stream_voice_audio_follows_output_format();
    err |= test_stream_voice_tells_the_chain_received_speech();
    err |= test_stream_encoder_monitor_plays_what_it_sends();
    err |= test_stream_encoder_mic_holds_while_unkeyed();
    err |= test_stream_encoder_mic_keeps_gain_across_key_ups();
    err |= test_stream_encoder_mic_starts_over_at_encoder_start();
    err |= test_stream_encoder_vox_keys_on_the_same_reads();
#endif
    err |= test_bert_payload_locks_from_default_state_and_continues();
    err |= test_bert_payload_resyncs_after_error_threshold();
    err |= test_bert_hard_payload_decode_primitives();
    err |= test_frame_info_packet_and_ip_helpers();
    err |= test_ip_frames_reject_truncation_before_reading_or_changing_state();
    err |= test_short_valid_ip_mpkt_reads_only_its_header();
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
    err |= test_stream_encoder_reads_stdin_samples();
    err |= test_stream_encoder_drops_audio_queued_before_a_rigctl_retune();
    err |= test_ip_decoder_propagates_udp_backend_failure();

    if (err == 0) {
        printf("M17_STATE_DISPATCH: OK\n");
    }
    return err;
}

// NOLINTEND(bugprone-implicit-widening-of-multiplication-result)
