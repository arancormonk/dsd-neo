// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*-------------------------------------------------------------------------------
 * m17.c
 * M17 Decoder and Encoder
 *
 * m17_scramble Bit Array from SDR++
 * CRC16, CSD encoder from libM17 / M17-Implementations (thanks again, sp5wwp)
 *
 *-----------------------------------------------------------------------------*/
#include <dsd-neo/core/bit_packing.h>

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/audio_filters.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/input_level.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/crypto/aes.h>
#include <dsd-neo/crypto/ecdsa.h>
#include <dsd-neo/fec/viterbi.h>
#include <dsd-neo/platform/audio.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/nonce.h>
#include <dsd-neo/protocol/m17/m17.h>
#include <dsd-neo/protocol/m17/m17_parse.h>
#include <dsd-neo/protocol/m17/m17_tables.h>
#include <dsd-neo/protocol/nxdn/nxdn_convolution.h>
#include <dsd-neo/runtime/control_pump.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/m17_udp_hooks.h>
#include <dsd-neo/runtime/net_audio_input_hooks.h>
#include <dsd-neo/runtime/rtl_stream_io_hooks.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/shutdown.h>
#include <dsd-neo/runtime/telemetry.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <math.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "m17_algorithms.h"
#include "m17_internal.h"
#include "m17_rrc_taps.h"

#ifdef USE_RADIO
#endif

#ifdef USE_CODEC2
#include <codec2/codec2.h>
#endif

static void decodeM17PKT(const dsd_opts* opts, dsd_state* state, const uint8_t* input, int len);
static void M17decodeLSF(dsd_state* state);
static void M17decodeLSFFields(dsd_state* state, const struct m17_lsf_result* res);
static void M17logLSFSummary(dsd_state* state, const struct m17_lsf_result* res);
static void M17storeLSFMeta(dsd_state* state, const struct m17_lsf_result* res);
static void M17decodeMetaPayload(dsd_state* state, uint8_t identifier);
static void M17decodeLSFMeta(dsd_state* state, const struct m17_lsf_result* res);
static void M17logLSFTrailer(const dsd_state* state, const struct m17_lsf_result* res);
static int m17_can_matches_state(const dsd_state* state);

static void
m17_write_wav_short_block(SNDFILE* file, const short* samples, sf_count_t sample_count, const char* context) {
    if (file == NULL || samples == NULL || sample_count <= 0) {
        return;
    }
    sf_count_t written = sf_write_short(file, samples, sample_count);
    if (written != sample_count) {
        LOG_WARN("%s: wrote %lld/%lld samples to WAV output", context, (long long)written, (long long)sample_count);
    }
}

#define M17_BASEBAND_SAMPLES ((size_t)M17_FRAME_SYMBOLS * (size_t)M17_RECOMMENDED_UPSAMPLE_FACTOR)
#define M17_BASEBAND_BYTES   (M17_BASEBAND_SAMPLES * sizeof(short))

enum {
    M17_SIGNATURE_VERIFY_NOT_RUN = 0,
    M17_SIGNATURE_VERIFY_VALID = 1,
    M17_SIGNATURE_VERIFY_INVALID = 2,
    M17_SIGNATURE_VERIFY_ERROR = 3,
    M17_SIGNATURE_VERIFY_NO_PUBLIC_KEY = 4,
};

short
m17_clip_float_to_short(float value) {
    if (value > 32767.0f) {
        return 32767;
    }
    if (value < -32768.0f) {
        return -32768;
    }
    return (short)lrintf(value);
}

static void
M17decodeCSD(dsd_state* state, unsigned long long int dst, unsigned long long int src) {
    //evaluate dst and src, and determine if they need to be converted to callsign
    DSD_MEMSET(state->m17_dst_csd, 0, sizeof(state->m17_dst_csd));
    DSD_MEMSET(state->m17_src_csd, 0, sizeof(state->m17_src_csd));
    const uint8_t dst_kind = m17_address_classify(dst);
    const uint8_t src_kind = m17_address_classify(src);
    if (dst_kind == M17_ADDRESS_BROADCAST_KIND) {
        DSD_FPRINTF(stderr, " DST: BROADCAST");
    } else if (dst_kind == M17_ADDRESS_EXTENDED) {
        DSD_FPRINTF(stderr, " DST: EXTENDED %012llx", dst);
    } else if (dst_kind == M17_ADDRESS_RESERVED) {
        DSD_FPRINTF(stderr, " DST: RESERVED %012llx", dst);
    } else {
        (void)m17_address_decode_csd(dst, state->m17_dst_csd);
        DSD_SNPRINTF(state->m17_dst_str, sizeof(state->m17_dst_str), "%s", state->m17_dst_csd);
        DSD_FPRINTF(stderr, " DST: %s", state->m17_dst_str);
    }

    if (src_kind == M17_ADDRESS_BROADCAST_KIND) {
        DSD_FPRINTF(stderr, " SRC:  UNKNOWN FFFFFFFFFFFF");
    } else if (src_kind == M17_ADDRESS_EXTENDED) {
        DSD_FPRINTF(stderr, " SRC: EXTENDED %012llx", src);
    } else if (src_kind == M17_ADDRESS_RESERVED) {
        DSD_FPRINTF(stderr, " SRC: RESERVED %012llx", src);
    } else {
        (void)m17_address_decode_csd(src, state->m17_src_csd);
        DSD_SNPRINTF(state->m17_src_str, sizeof(state->m17_src_str), "%s", state->m17_src_csd);
        DSD_FPRINTF(stderr, " SRC: %s", state->m17_src_str);
    }

    //debug
}

uint16_t
m17_compose_frame_info(uint16_t ps, uint16_t dt, uint16_t et, uint16_t es, uint16_t cn, uint16_t signature,
                       uint16_t reserved) {
    return (uint16_t)((ps & 0x1U) | ((dt & 0x3U) << 1) | ((et & 0x3U) << 3) | ((es & 0x3U) << 5) | ((cn & 0xFU) << 7)
                      | ((signature & 0x1U) << 11) | ((reserved & 0xFU) << 12));
}

static void
M17logDataType(uint8_t dt) {
    switch (dt) {
        case 0: LOG_INFO(" Reserved"); break;
        case 1: LOG_INFO(" Data"); break;
        case 2: LOG_INFO(" Voice (3200bps)"); break;
        case 3: LOG_INFO(" Voice (1600bps)"); break;
        default: break;
    }
}

static void
M17logEncryption(uint8_t et, uint8_t es) {
    if (et == 0U) {
        return;
    }

    LOG_INFO(" ENC:");
    switch (et) {
        case 1: LOG_INFO(" Scrambler - %d", es); break;
        case 2: LOG_INFO(" AES-CTR"); break;
        default: break;
    }
}

static uint8_t
M17streamDataTypeFromLSF(const struct m17_lsf_result* res) {
    if (res->packet_stream == 0U) {
        return 20U;
    }
    return res->dt;
}

static void
M17printLICH(const uint8_t* lich_decoded) {
    DSD_FPRINTF(stderr, " LICH: ");
    for (int i = 0; i < 6; i++) {
        DSD_FPRINTF(stderr, "[%02X]", (uint8_t)convert_bits_into_output(&lich_decoded[((size_t)i * 8)], 8));
    }
}

static void
M17printLSF(const uint8_t* lsf_packed, uint16_t crc_ext, uint16_t crc_cmp) {
    DSD_FPRINTF(stderr, "\n LSF: ");
    for (int i = 0; i < 30; i++) {
        if (i == 15) {
            DSD_FPRINTF(stderr, "\n      ");
        }
        DSD_FPRINTF(stderr, "[%02X]", lsf_packed[i]);
    }
    DSD_FPRINTF(stderr, "\n      (CRC CHK) E: %04X; C: %04X;", crc_ext, crc_cmp);
}

unsigned long long
m17_read_ip_source(const uint8_t* ip_frame) {
    if (ip_frame == NULL) {
        return 0ULL;
    }
    return ((unsigned long long int)ip_frame[4] << 40ULL) + ((unsigned long long int)ip_frame[5] << 32ULL)
           + ((unsigned long long int)ip_frame[6] << 24ULL) + ((unsigned long long int)ip_frame[7] << 16ULL)
           + ((unsigned long long int)ip_frame[8] << 8ULL) + ((unsigned long long int)ip_frame[9] << 0ULL);
}

static void
M17printIpSource(unsigned long long int src) {
    const uint8_t kind = m17_address_classify(src);
    if (kind == M17_ADDRESS_BROADCAST_KIND) {
        DSD_FPRINTF(stderr, "UNKNOWN FFFFFFFFFFFF");
    } else if (kind == M17_ADDRESS_EXTENDED) {
        DSD_FPRINTF(stderr, "EXTENDED %012llx", src);
    } else if (kind == M17_ADDRESS_RESERVED) {
        DSD_FPRINTF(stderr, "RESERVED %012llx", src);
    } else {
        char csd[10];
        (void)m17_address_decode_csd(src, csd);
        DSD_FPRINTF(stderr, "%s", csd);
    }
}

static void
M17finalizeLICH(dsd_state* state, const dsd_opts* opts) {
    uint8_t lsf_packed[M17_LSF_BYTES];
    DSD_MEMSET(lsf_packed, 0, sizeof(lsf_packed));

    //need to pack bytes for the sw5wwp variant of the crc (might as well, may be useful in the future)
    for (int i = 0; i < M17_LSF_BYTES; i++) {
        lsf_packed[i] = (uint8_t)convert_bits_into_output(&state->m17_lsf[((size_t)i * 8)], 8);
    }

    const uint16_t crc_cmp = m17_crc16(lsf_packed, M17_LSF_LSD_BYTES);
    const uint16_t crc_ext = (uint16_t)convert_bits_into_output(&state->m17_lsf[M17_LSF_LSD_BITS], M17_LSF_CRC_BITS);
    const uint8_t crc_err = (crc_cmp != crc_ext) ? 1U : 0U;

    if (crc_err == 0 || opts->aggressive_framesync == 0) {
        M17decodeLSF(state);
    }

    if (opts->payload == 1) {
        M17printLSF(lsf_packed, crc_ext, crc_cmp);
    }

    DSD_MEMSET(state->m17_lsf, 0, sizeof(state->m17_lsf));

    if (crc_err != 0U) {
        DSD_FPRINTF(stderr, " EMB LSF CRC ERR");
    }
}

static void
M17decodeLSF(dsd_state* state) {
    struct m17_lsf_result res;
    if (m17_parse_lsf(state->m17_lsf, sizeof(state->m17_lsf), &res) != 0) {
        LOG_WARN("M17: failed to parse LSF\n");
        return;
    }

    if (m17_apply_lsf_result(state, &res) == 1) {
        M17logLSFTrailer(state, &res);
    }
}

static void
M17decodeLSFFields(dsd_state* state, const struct m17_lsf_result* res) {
    //store this so we can reference it for playing voice and/or decoding data, dst/src etc
    state->m17_str_dt = M17streamDataTypeFromLSF(res);
    state->m17_dst = res->dst;
    state->m17_src = res->src;
    state->m17_can = res->cn;
    state->m17_enc = res->et;
    state->m17_enc_st = res->es;
    state->m17_payload_decrypted = 0U;
    state->m17_signature_advertised = res->signature;
    DSD_MEMSET(state->m17_signature_digest, 0, sizeof(state->m17_signature_digest));
    DSD_MEMSET(state->m17_signature, 0, sizeof(state->m17_signature));
    state->m17_signature_received_mask = 0U;
    state->m17_signature_complete = 0U;
    state->m17_signature_bad_sequence = 0U;
    state->m17_signature_verification_status = M17_SIGNATURE_VERIFY_NOT_RUN;
}

static void
M17logLSFSummary(dsd_state* state, const struct m17_lsf_result* res) {
    /* Preserve the established log format while routing through LOG_* macros. */
    LOG_INFO("\n");

    LOG_INFO(" CAN: %d", res->cn);
    M17decodeCSD(state, res->dst, res->src);

    M17logDataType(res->dt);

    if (res->signature != 0U) {
        LOG_INFO(" Signed (secp256r1);");
    } else if (res->rs != 0U) {
        LOG_INFO(" RS: %02X", res->rs);
    }
    LOG_INFO("\n");
    M17logEncryption(res->et, res->es);
}

static void
M17storeLSFMeta(dsd_state* state, const struct m17_lsf_result* res) {
    //compare incoming META/IV value on AES, if timestamp 32-bits are not within a time 5 minute window, then throw a warning
    // long long int epoch = 1577836800LL;                                     //Jan 1, 2020, 00:00:00 UTC
    // uint32_t tsn = ( (time(NULL)-epoch) & 0xFFFFFFFF); //current LSB 32-bit value
    // uint32_t tsi = (uint32_t)convert_bits_into_output(&state->m17_lsf[112], 32); //OTA LSB 32-bit value
    // uint32_t dif = abs(tsn-tsi);

    //debug

    //pack meta bits into 14 bytes, using state->m17_meta as the AES-IV buffer for M17
    DSD_MEMSET(state->m17_meta, 0, sizeof(state->m17_meta));
    if (res->has_meta != 0U || res->meta_is_iv != 0U) {
        DSD_MEMCPY(state->m17_meta, res->meta, sizeof(res->meta));
    }
}

size_t
m17_encode_packet_protocol_id(uint32_t identifier, uint8_t* out) {
    if (out == NULL || identifier > M17_PACKET_PROTOCOL_MAX) {
        return 0U;
    }

    if (identifier < 0x80U) {
        out[0] = (uint8_t)identifier;
        return 1U;
    }
    if (identifier < 0x800U) {
        out[0] = (uint8_t)(0xC0U | ((identifier >> 6U) & 0x1FU));
        out[1] = (uint8_t)(0x80U | (identifier & 0x3FU));
        return 2U;
    }
    if (identifier < 0x10000U) {
        out[0] = (uint8_t)(0xE0U | ((identifier >> 12U) & 0x0FU));
        out[1] = (uint8_t)(0x80U | ((identifier >> 6U) & 0x3FU));
        out[2] = (uint8_t)(0x80U | (identifier & 0x3FU));
        return 3U;
    }

    out[0] = (uint8_t)(0xF0U | ((identifier >> 18U) & 0x07U));
    out[1] = (uint8_t)(0x80U | ((identifier >> 12U) & 0x3FU));
    out[2] = (uint8_t)(0x80U | ((identifier >> 6U) & 0x3FU));
    out[3] = (uint8_t)(0x80U | (identifier & 0x3FU));
    return 4U;
}

static void
M17decodeMetaPayload(dsd_state* state, uint8_t identifier) {
    uint8_t meta[4U + M17_META_BYTES];
    DSD_MEMSET(meta, 0, sizeof(meta));
    const size_t protocol_len = m17_encode_packet_protocol_id(identifier, meta);
    if (protocol_len == 0U) {
        return;
    }
    DSD_MEMCPY(meta + protocol_len, state->m17_meta, M17_META_BYTES);
    LOG_INFO("\n ");
    //Note: We don't have opts here, so in the future, if we need it, we will need to pass it here
    decodeM17PKT(NULL, state, meta, (int)(protocol_len + M17_META_BYTES)); //decode META
}

static void
M17decodeLSFMeta(dsd_state* state, const struct m17_lsf_result* res) {
    //Decode Meta Data when not ENC (if meta field is populated with something)
    if (res->et == 0U && res->has_meta != 0U) {
        if (!m17_can_matches_state(state)) {
            LOG_INFO(" META CAN Filtered;");
            return;
        }
        const uint8_t identifier = m17_null_meta_protocol_for_subtype(res->es);
        if (identifier != 0U) {
            M17decodeMetaPayload(state, identifier);
        } else {
            LOG_INFO(" Reserved META subtype;");
        }
    }
}

static void
M17logLSFTrailer(const dsd_state* state, const struct m17_lsf_result* res) {
    // If no Meta (debug)
    if (res->et == 2) {
        LOG_INFO(" IV: ");
        for (int i = 0; i < 16; i++) {
            LOG_INFO("%02X", state->m17_meta[i]);
        }
    }

    LOG_INFO("\n FT: %04X; P/S: %X; DT: %X; ET: %X; ES: %X; SIG: %X; RS: %X;", res->type_word, res->packet_stream,
             res->dt, res->et, res->es, res->signature, res->rs);
}

int
m17_process_lich(dsd_state* state, const dsd_opts* opts, const uint8_t* lich_bits) {
    if (state == NULL || opts == NULL || lich_bits == NULL) {
        return -1;
    }
    uint8_t lich_decoded[M17_LICH_CONTENT_BITS];
    DSD_MEMSET(lich_decoded, 0, sizeof(lich_decoded));

    int err = m17_lich_decode_bits(lich_bits, lich_decoded);
    uint8_t lich_counter = 0U;
    uint8_t lich_reserve = 0U;
    if (m17_lich_parse_content(lich_decoded, &lich_counter, &lich_reserve) != 0) {
        err = -1;
    }

    if (err == 0) {
        DSD_FPRINTF(stderr, "LC: %d/6 ", lich_counter + 1);
    } else {
        DSD_FPRINTF(stderr, "LICH G24 ERR");
    }

    if (err == 0) {
        if (lich_reserve != 0U) {
            DSD_FPRINTF(stderr, "LICH RSV:%02X ", lich_reserve);
        }
        for (int i = 0; i < M17_LICH_CHUNK_BITS; i++) {
            state->m17_lsf[((size_t)lich_counter * M17_LICH_CHUNK_BITS) + i] = lich_decoded[i];
        }
    }

    if (opts->payload == 1) {
        M17printLICH(lich_decoded);
    }

    if (err == 0 && lich_counter == (M17_LICH_CHUNKS - 1U)) {
        M17finalizeLICH(state, opts);
    }

    return err;
}

static void
m17_unpack_voice_octets(const uint8_t* payload, unsigned char* voice1, unsigned char* voice2) {
    for (int i = 0; i < 8; i++) {
        voice1[i] = (unsigned char)convert_bits_into_output(&payload[((size_t)i * 8) + 0], 8);
        voice2[i] = (unsigned char)convert_bits_into_output(&payload[((size_t)i * 8) + 64], 8);
    }
}

static void
m17_print_codec_line(const char* label, const unsigned char* bytes) {
    DSD_FPRINTF(stderr, "%s", label);
    for (int i = 0; i < 8; i++) {
        DSD_FPRINTF(stderr, "%02X", bytes[i]);
    }
}

static int
m17_any_nonzero_octets(const uint8_t* bytes, int len) {
    for (int i = 0; i < len; i++) {
        if (bytes[i] != 0U) {
            return 1;
        }
    }
    return 0;
}

static int
m17_load_aes_key(const dsd_state* state, uint8_t subtype, uint8_t key[32]) {
    if (state == NULL || key == NULL) {
        return 0;
    }

    DSD_MEMSET(key, 0, 32U);
    const uint8_t key_bytes = m17_aes_key_bytes_for_subtype(subtype);
    if (key_bytes == 0U) {
        return 0;
    }

    if (state->aes_key_loaded[0] == 1) {
        if (state->aes_key_segments[0] != 0U && ((uint8_t)(state->aes_key_segments[0] * 8U) < key_bytes)) {
            return 0;
        }
        DSD_MEMCPY(key, state->aes_key, key_bytes);
        return m17_any_nonzero_octets(key, key_bytes);
    }

    return 0;
}

static void
m17_store_current_aes_counter(dsd_state* state, uint16_t frame_number) {
    uint8_t counter[M17_AES_COUNTER_BYTES];
    DSD_MEMSET(counter, 0, sizeof(counter));
    m17_aes_build_counter(state->m17_meta, frame_number, counter);
    state->m17_meta[14] = counter[14];
    state->m17_meta[15] = counter[15];
}

static int
m17_decrypt_stream_payload(dsd_state* state, uint16_t frame_number, const uint8_t* input_bits, uint8_t* output_bits) {
    if (state == NULL || input_bits == NULL || output_bits == NULL) {
        return 0;
    }

    if (state->m17_enc == 0U) {
        DSD_MEMCPY(output_bits, input_bits, M17_STREAM_PAYLOAD_BITS);
        return 1;
    }

    if (state->m17_enc == 1U) {
        const uint32_t seed = (uint32_t)state->R & m17_scrambler_mask_for_subtype(state->m17_enc_st);
        return m17_scrambler_apply_bits(state->m17_enc_st, seed, frame_number, input_bits, output_bits,
                                        M17_STREAM_PAYLOAD_BITS)
               == 0;
    }

    if (state->m17_enc == 2U) {
        uint8_t key[32];
        uint8_t counter[M17_AES_COUNTER_BYTES];
        uint8_t payload[M17_SIGNATURE_DIGEST_BYTES];
        if (!m17_load_aes_key(state, state->m17_enc_st, key)) {
            return 0;
        }
        m17_aes_build_counter(state->m17_meta, frame_number, counter);
        pack_bit_array_into_byte_array(input_bits, payload, (int)sizeof(payload));
        const dsd_aes_key_size key_size = (state->m17_enc_st == 0U)   ? DSD_AES_KEY_128
                                          : (state->m17_enc_st == 1U) ? DSD_AES_KEY_192
                                                                      : DSD_AES_KEY_256;
        aes_ctr_xcrypt_bytes(counter, key, payload, key_size, sizeof(payload));
        unpack_byte_array_into_bit_array(payload, output_bits, (int)sizeof(payload));
        return 1;
    }

    return 0;
}

static int
m17_can_matches_state(const dsd_state* state) {
    return state == NULL || m17_can_filter_allows(state->m17_can_en, state->m17_can);
}

#ifdef USE_CODEC2
static int
m17_can_emit_audio(const dsd_opts* opts, const dsd_state* state) {
    return (opts->slot1_on == 1 && (state->m17_enc == 0 || state->m17_payload_decrypted != 0U)
            && m17_can_matches_state(state));
}

static void
m17_write_decoded_audio_single(const dsd_opts* opts, dsd_state* state, const short* samples, size_t nsam,
                               const char* log_ctx) {
    if (!m17_can_emit_audio(opts, state)) {
        return;
    }

    if (opts->audio_out_type == 0) {
        dsd_audio_write(opts->audio_out_stream, samples, nsam);
        return;
    }

    if (opts->audio_out_type == 8) {
        dsd_udp_audio_hook_blast(opts, state, nsam * sizeof(short), (short*)samples);
        return;
    }

    if (opts->audio_out_type == 1) {
        const ssize_t written = dsd_write(opts->audio_out_fd, samples, nsam * sizeof(short));
        if (written < 0) {
            LOG_WARN("%s: failed to write %zu-byte audio block", log_ctx, nsam * sizeof(short));
        }
    }
}

static void
m17_write_decoded_audio_pair(const dsd_opts* opts, dsd_state* state, const short* first, const short* second,
                             size_t nsam, const char* log_ctx) {
    if (!m17_can_emit_audio(opts, state)) {
        return;
    }

    if (opts->audio_out_type == 0) {
        dsd_audio_write(opts->audio_out_stream, first, nsam);
        dsd_audio_write(opts->audio_out_stream, second, nsam);
        return;
    }

    if (opts->audio_out_type == 8) {
        dsd_udp_audio_hook_blast(opts, state, nsam * sizeof(short), (short*)first);
        dsd_udp_audio_hook_blast(opts, state, nsam * sizeof(short), (short*)second);
        return;
    }

    if (opts->audio_out_type == 1) {
        ssize_t written = dsd_write(opts->audio_out_fd, first, nsam * sizeof(short));
        if (written < 0) {
            LOG_WARN("%s: failed to write first %zu-byte audio block", log_ctx, nsam * sizeof(short));
        }
        written = dsd_write(opts->audio_out_fd, second, nsam * sizeof(short));
        if (written < 0) {
            LOG_WARN("%s: failed to write second %zu-byte audio block", log_ctx, nsam * sizeof(short));
        }
    }
}

static void
m17_write_static_stereo_wav(const short* mono, int count, SNDFILE* wav_out_f) {
    short stereo[320 * 2];
    for (int i = 0; i < count; i++) {
        stereo[((size_t)i * 2) + 0] = mono[i];
        stereo[((size_t)i * 2) + 1] = mono[i];
    }
    m17_write_wav_short_block(wav_out_f, stereo, ((sf_count_t)count) * 2, "M17 static stereo WAV");
}

static void
m17_maybe_write_wav_single(const dsd_opts* opts, const dsd_state* state, const short* samples, int count) {
    if (opts->wav_out_f == NULL || (state->m17_enc != 0 && state->m17_payload_decrypted == 0U)) {
        return;
    }

    if (opts->dmr_stereo_wav == 1) {
        m17_write_wav_short_block(opts->wav_out_f, samples, count, "M17 WAV single");
        return;
    }

    if (opts->static_wav_file == 1) {
        m17_write_static_stereo_wav(samples, count, opts->wav_out_f);
    }
}

static void
m17_maybe_write_wav_pair(const dsd_opts* opts, const dsd_state* state, const short* first, const short* second,
                         int count) {
    if (opts->wav_out_f == NULL || (state->m17_enc != 0 && state->m17_payload_decrypted == 0U)) {
        return;
    }

    if (opts->dmr_stereo_wav == 1) {
        m17_write_wav_short_block(opts->wav_out_f, first, count, "M17 WAV pair first");
        m17_write_wav_short_block(opts->wav_out_f, second, count, "M17 WAV pair second");
        return;
    }

    if (opts->static_wav_file == 1) {
        m17_write_static_stereo_wav(first, count, opts->wav_out_f);
        m17_write_static_stereo_wav(second, count, opts->wav_out_f);
    }
}
#endif

static void
M17processCodec2_1600(const dsd_opts* opts, dsd_state* state, const uint8_t* payload, uint16_t frame_number) {

    unsigned char voice1[8];
    unsigned char voice2[8];
    m17_unpack_voice_octets(payload, voice1, voice2);

    (void)state->m17_enc;

    if (opts->payload == 1) {
        m17_print_codec_line("\n CODEC2: ", voice1);
        DSD_FPRINTF(stderr, " (1600)");
        m17_print_codec_line("\n A_DATA: ", voice2); //arbitrary data
    }

#ifdef USE_CODEC2
    const size_t nsam = 320;

    /* Use fixed-size stack buffers to avoid per-frame heap churn */
    short samp1[320];
    codec2_decode(state->codec2_1600, samp1, voice1);

    if (opts->use_hpf_d == 1) {
        hpf_dL(state, samp1, nsam);
    }

    m17_write_decoded_audio_single(opts, state, samp1, nsam, "M17processCodec2_1600");
    m17_maybe_write_wav_single(opts, state, samp1, (int)nsam);

#endif

    //handle arbitrary data
    uint8_t adata[9];
    adata[0] = 0x89; //set so pkt decoder will rip these out as just utf-8 chars
    for (int i = 0; i < 8; i++) {
        adata[i + 1] = (unsigned char)convert_bits_into_output(&payload[((size_t)i * 8) + 64], 8);
    }

    if (m17_any_nonzero_octets(adata + 1, 8)) {
        DSD_FPRINTF(stderr, "\n");           //linebreak
        decodeM17PKT(opts, state, adata, 9); //decode Arbitrary Data as UTF-8
    }

    uint8_t aggregate[49];
    DSD_MEMSET(aggregate, 0, sizeof(aggregate));
    const int assembled =
        m17_stream_1600_arbitrary_assemble(state->dmr_pdu_sf[0], frame_number, (const uint8_t*)voice2, aggregate);
    if (assembled > 0 && m17_any_nonzero_octets(aggregate + 1, 48)) {
        DSD_FPRINTF(stderr, "\n");
        decodeM17PKT(opts, state, aggregate, 49);
    }
}

static void
// cppcheck-suppress constParameterPointer ; state is mutable when USE_CODEC2 enables HPF state updates
M17processCodec2_3200(const dsd_opts* opts, dsd_state* state, const uint8_t* payload) {
    unsigned char voice1[8];
    unsigned char voice2[8];
    m17_unpack_voice_octets(payload, voice1, voice2);

    (void)state->m17_enc;

    if (opts->payload == 1) {
        m17_print_codec_line("\n CODEC2: ", voice1);
        DSD_FPRINTF(stderr, " (3200)");
        m17_print_codec_line("\n CODEC2: ", voice2);
        DSD_FPRINTF(stderr, " (3200)");
    }

#ifdef USE_CODEC2
    const size_t nsam = 160;

    /* Use fixed-size stack buffers to avoid per-frame heap churn */
    short samp1[160];
    short samp2[160];

    codec2_decode(state->codec2_3200, samp1, voice1);
    codec2_decode(state->codec2_3200, samp2, voice2);

    if (opts->use_hpf_d == 1) {
        hpf_dL(state, samp1, nsam);
        hpf_dL(state, samp2, nsam);
    }

    m17_write_decoded_audio_pair(opts, state, samp1, samp2, nsam, "M17processCodec2_3200");
    m17_maybe_write_wav_pair(opts, state, samp1, samp2, (int)nsam);

#endif
}

static void
M17printStreamBits(const uint8_t* trellis_buf) {
    DSD_FPRINTF(stderr, "\n STREAM: ");
    for (int i = 0; i < 18; i++) {
        DSD_FPRINTF(stderr, "[%02X]", (uint8_t)convert_bits_into_output(&trellis_buf[((size_t)i * 8)], 8));
    }
}

static void
M17printSignatureBits(const uint8_t* trellis_buf) {
    DSD_FPRINTF(stderr, "\n SIG: ");
    for (int i = 2; i < 18; i++) {
        DSD_FPRINTF(stderr, "[%02X]", (uint8_t)convert_bits_into_output(&trellis_buf[((size_t)i * 8)], 8));
    }
}

static void
M17verifyCollectedSignature(dsd_state* state) {
    if (state->m17_signature_complete == 0U
        || state->m17_signature_verification_status != M17_SIGNATURE_VERIFY_NOT_RUN) {
        return;
    }

    if (state->m17_signature_public_key_loaded == 0U) {
        state->m17_signature_verification_status = M17_SIGNATURE_VERIFY_NO_PUBLIC_KEY;
        DSD_FPRINTF(stderr, " NO PUBKEY;");
        return;
    }

    const int verify_rc = dsd_ecdsa_p256_verify_digest(state->m17_signature_digest, sizeof(state->m17_signature_digest),
                                                       state->m17_signature_public_key, state->m17_signature);
    if (verify_rc == 1) {
        state->m17_signature_verification_status = M17_SIGNATURE_VERIFY_VALID;
        DSD_FPRINTF(stderr, " VERIFIED;");
    } else if (verify_rc == 0) {
        state->m17_signature_verification_status = M17_SIGNATURE_VERIFY_INVALID;
        DSD_FPRINTF(stderr, " VERIFY FAIL;");
    } else {
        state->m17_signature_verification_status = M17_SIGNATURE_VERIFY_ERROR;
        DSD_FPRINTF(stderr, " VERIFY ERR;");
    }
}

static int
M17collectSignaturePayload(dsd_state* state, const uint8_t* payload_bytes, const uint8_t* trellis_buf,
                           uint16_t frame_number) {
    if (state->m17_signature_advertised == 0U || m17_stream_signature_frame_index(frame_number) < 0
        || (state->m17_str_dt != 2U && state->m17_str_dt != 3U)) {
        return 0;
    }

    struct m17_signature_collector collector;
    DSD_MEMCPY(collector.signature, state->m17_signature, sizeof(collector.signature));
    collector.received_mask = state->m17_signature_received_mask;
    collector.complete = state->m17_signature_complete;
    collector.bad_sequence = state->m17_signature_bad_sequence;

    const int collected = m17_signature_collector_push(&collector, frame_number, payload_bytes);
    DSD_MEMCPY(state->m17_signature, collector.signature, sizeof(state->m17_signature));
    state->m17_signature_received_mask = collector.received_mask;
    state->m17_signature_complete = collector.complete;
    state->m17_signature_bad_sequence = collector.bad_sequence;

    M17printSignatureBits(trellis_buf);
    if (collected > 0) {
        DSD_FPRINTF(stderr, " COMPLETE;");
        M17verifyCollectedSignature(state);
    } else if (collected < 0) {
        DSD_FPRINTF(stderr, " SEQUENCE ERR;");
    }
    return 1;
}

static void
M17updateSignatureDigestIfNeeded(dsd_state* state, const uint8_t* payload_bytes, uint16_t payload_frame_number) {
    if (state->m17_signature_advertised != 0U && payload_frame_number < M17_STREAM_SIGNATURE_FN0) {
        m17_signature_digest_update(state->m17_signature_digest, payload_bytes);
    }
}

static void
M17processStreamPayloadBits(const dsd_opts* opts, dsd_state* state, const uint8_t* processed_payload,
                            uint16_t payload_frame_number) {
    switch (state->m17_str_dt) {
        case 2: M17processCodec2_3200(opts, state, processed_payload); break;
        case 3: M17processCodec2_1600(opts, state, processed_payload, payload_frame_number); break;
        case 1: DSD_FPRINTF(stderr, " DATA;"); break;
        case 0: DSD_FPRINTF(stderr, "  RES;"); break;
        default: break;
    }
}

int
m17_dispatch_stream_payload(const dsd_opts* opts, dsd_state* state, const uint8_t* payload, uint16_t frame_number,
                            uint8_t* processed_payload) {
    if (opts == NULL || state == NULL || payload == NULL || processed_payload == NULL) {
        return M17_STREAM_INVALID;
    }

    uint8_t payload_bytes[M17_SIGNATURE_DIGEST_BYTES];
    uint8_t trellis_buf[M17_STREAM_TYPE1_FLUSH_BITS];
    DSD_MEMSET(payload_bytes, 0, sizeof(payload_bytes));
    DSD_MEMSET(trellis_buf, 0, sizeof(trellis_buf));
    DSD_MEMSET(processed_payload, 0, M17_STREAM_PAYLOAD_BITS);
    pack_bit_array_into_byte_array(payload, payload_bytes, (int)sizeof(payload_bytes));
    m17_stream_build_type1_bits(frame_number, payload, trellis_buf);

    if (M17collectSignaturePayload(state, payload_bytes, trellis_buf, frame_number) != 0) {
        return M17_STREAM_SIGNATURE_CONSUMED;
    }

    const uint16_t payload_frame_number = (uint16_t)(frame_number & M17_STREAM_FRAME_COUNTER_MAX);
    M17updateSignatureDigestIfNeeded(state, payload_bytes, payload_frame_number);

    if (!m17_can_matches_state(state)) {
        DSD_FPRINTF(stderr, " CAN Filtered;");
        return M17_STREAM_CAN_FILTERED;
    }

    const int payload_ready = m17_decrypt_stream_payload(state, frame_number, payload, processed_payload);
    if (payload_ready == 0) {
        DSD_FPRINTF(stderr, " *Encrypted*");
        return M17_STREAM_ENCRYPTED_LOCKED;
    }

    const uint8_t old_payload_decrypted = state->m17_payload_decrypted;
    state->m17_payload_decrypted = (state->m17_enc != 0U) ? 1U : 0U;
    M17processStreamPayloadBits(opts, state, processed_payload, payload_frame_number);
    const int result = (state->m17_enc != 0U) ? M17_STREAM_ENCRYPTED_DISPATCHED : M17_STREAM_CLEAR_DISPATCHED;
    state->m17_payload_decrypted = old_payload_decrypted;

    if (opts->payload == 1 && state->m17_str_dt < 2) {
        M17printStreamBits(trellis_buf);
    }
    return result;
}

int
m17_apply_lsf_result(dsd_state* state, const struct m17_lsf_result* res) {
    if (state == NULL || res == NULL) {
        return -1;
    }
    if (res->rs != 0U) {
        LOG_INFO(" Unknown LSF TYPE;");
        return 0;
    }
    if (res->type_reserved_valid == 0U) {
        LOG_INFO(" Reserved LSF TYPE fields;");
        return 0;
    }
    if (res->dst_is_valid == 0U) {
        LOG_INFO(" Invalid DST address;");
        return 0;
    }
    if (res->src_is_valid == 0U) {
        LOG_INFO(" Invalid SRC address for transmitting station;");
        return 0;
    }

    M17decodeLSFFields(state, res);
    M17logLSFSummary(state, res);
    M17storeLSFMeta(state, res);
    M17decodeLSFMeta(state, res);
    return 1;
}

static void
M17prepareStream(const dsd_opts* opts, dsd_state* state, const uint8_t* m17_bits) {

    int i, k, x;
    uint8_t m17_punc[275]; //25 * 11 = 275
    DSD_MEMSET(m17_punc, 0, sizeof(m17_punc));
    for (i = 0; i < 272; i++) {
        m17_punc[i] = m17_bits[i + 96];
    }

    //depuncture the bits
    uint8_t m17_depunc[300]; //25 * 12 = 300
    DSD_MEMSET(m17_depunc, 0, sizeof(m17_depunc));
    k = 0;
    x = 0;
    for (i = 0; i < 25; i++) {
        m17_depunc[k++] = m17_punc[x++];
        m17_depunc[k++] = m17_punc[x++];
        m17_depunc[k++] = m17_punc[x++];
        m17_depunc[k++] = m17_punc[x++];
        m17_depunc[k++] = m17_punc[x++];
        m17_depunc[k++] = m17_punc[x++];
        m17_depunc[k++] = m17_punc[x++];
        m17_depunc[k++] = m17_punc[x++];
        m17_depunc[k++] = m17_punc[x++];
        m17_depunc[k++] = m17_punc[x++];
        m17_depunc[k++] = m17_punc[x++];
        m17_depunc[k++] = 0;
    }

    //setup the convolutional decoder
    uint8_t temp[300];
    uint8_t m_data[28];
    uint8_t trellis_buf[144];
    DSD_MEMSET(trellis_buf, 0, sizeof(trellis_buf));
    DSD_MEMSET(temp, 0, sizeof(temp));
    DSD_MEMSET(m_data, 0, sizeof(m_data));

    for (i = 0; i < 296; i++) {
        temp[i] = m17_depunc[i] << 1;
    }

    CNXDNConvolution_start();
    for (i = 0; i < 148; i++) {
        const uint8_t s0 = temp[((size_t)2 * i)];
        const uint8_t s1 = temp[((size_t)2 * i) + 1];

        CNXDNConvolution_decode(s0, s1);
    }

    CNXDNConvolution_chainback(m_data, 144);

    //144/8 = 18, last 4 (144-148) are trailing zeroes
    unpack_byte_array_into_bit_array(m_data, trellis_buf, 18);

    //load m_data into bits for either data packets or voice packets
    uint8_t payload[128];
    DSD_MEMSET(payload, 0, sizeof(payload));

    uint16_t stream_frame_number = 0U;
    (void)m17_stream_parse_type1_bits(trellis_buf, &stream_frame_number, payload);
    const uint8_t end = (uint8_t)((stream_frame_number & M17_STREAM_FRAME_END_MASK) != 0U);
    const uint16_t fn = (uint16_t)(stream_frame_number & M17_STREAM_FRAME_COUNTER_MAX);

    m17_store_current_aes_counter(state, stream_frame_number);

    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, " FSN: %05d", fn);
    }

    if (end == 1) {
        DSD_FPRINTF(stderr, " END;");
    }

    uint8_t processed_payload[M17_STREAM_PAYLOAD_BITS];
    (void)m17_dispatch_stream_payload((const dsd_opts*)opts, state, payload, stream_frame_number, processed_payload);
}

void
processM17STR(dsd_opts* opts, dsd_state* state) {

    int i;
    uint8_t dbuf[384];         //384-bit frame - 16-bit (8 symbol) sync pattern (184 dibits)
    uint8_t m17_rnd_bits[368]; //368 bits that are still scrambled (randomized)
    uint8_t m17_int_bits[368]; //368 bits that are still interleaved
    uint8_t m17_bits[368];     //368 bits that have been de-interleaved and de-scramble
    uint8_t lich_bits[96];
    int lich_err = -1;

    DSD_MEMSET(dbuf, 0, sizeof(dbuf));
    DSD_MEMSET(m17_rnd_bits, 0, sizeof(m17_rnd_bits));
    DSD_MEMSET(m17_int_bits, 0, sizeof(m17_int_bits));
    DSD_MEMSET(m17_bits, 0, sizeof(m17_bits));
    DSD_MEMSET(lich_bits, 0, sizeof(lich_bits));

    //load dibits into dibit buffer
    for (i = 0; i < 184; i++) {
        dbuf[i] = (uint8_t)get_dibit_and_analog_signal(opts, state, NULL);
    }

    //convert dbuf into a bit array
    for (i = 0; i < 184; i++) {
        m17_rnd_bits[i * 2 + 0] = (dbuf[i] >> 1) & 1;
        m17_rnd_bits[i * 2 + 1] = (dbuf[i] >> 0) & 1;
    }

    //descramble the frame
    for (i = 0; i < 368; i++) {
        m17_int_bits[i] = (m17_rnd_bits[i] ^ m17_scramble[i]) & 1;
    }

    //deinterleave the bit array using Quadratic Permutation Polynomial
    //function π(x) = (45x + 92x^2 ) mod 368
    for (i = 0; i < 368; i++) {
        const int x = ((45 * i) + (92 * i * i)) % 368;
        m17_bits[i] = m17_int_bits[x];
    }

    for (i = 0; i < 96; i++) {
        lich_bits[i] = m17_bits[i];
    }

    //check lich first, and handle LSF chunk and completed LSF
    lich_err = m17_process_lich(state, opts, lich_bits);

    if (lich_err == 0) {
        M17prepareStream(opts, state, m17_bits);
    }

    //ending linebreak
    DSD_FPRINTF(stderr, "\n");

} //end processM17STR

static void
m17_capture_soft_symbols(dsd_opts* opts, dsd_state* state, float* soft_symbols) {
    soft_symbol_frame_begin(state);
    for (int i = 0; i < 184; i++) {
        (void)getDibitAndSoftSymbol(opts, state, &soft_symbols[i]);
    }
}

static void
m17_soft_bits_from_symbols(const float* soft_symbols, const dsd_state* state, uint16_t* soft_bits) {
    uint16_t soft_rnd[368];
    uint16_t soft_int[368];

    for (int i = 0; i < 184; i++) {
        soft_rnd[i * 2 + 0] = soft_symbol_to_viterbi_cost(soft_symbols[i], state, 0);
        soft_rnd[i * 2 + 1] = soft_symbol_to_viterbi_cost(soft_symbols[i], state, 1);
    }

    for (int i = 0; i < 368; i++) {
        soft_int[i] = m17_scramble[i] ? (uint16_t)(0xFFFFU - soft_rnd[i]) : soft_rnd[i];
    }

    for (int i = 0; i < 368; i++) {
        const int x = ((45 * i) + (92 * i * i)) % 368;
        soft_bits[i] = soft_int[x];
    }
}

static void
m17_soft_depuncture_p1(const uint16_t* soft_bits, uint16_t* depunc) {
    int bit_index = 0;
    for (int i = 0; i < 488; i++) {
        if (m17_puncture_pattern_1[i % M17_PUNCTURE_P1_LEN] == 1) {
            depunc[i] = soft_bits[bit_index++];
        } else {
            depunc[i] = 0x7FFF;
        }
    }
}

static void
m17_read_payload_randomized_bits(dsd_opts* opts, dsd_state* state, uint8_t* out_bits) {
    uint8_t dbuf[M17_PAYLOAD_SYMBOLS];
    DSD_MEMSET(dbuf, 0, sizeof(dbuf));
    for (int i = 0; i < M17_PAYLOAD_SYMBOLS; i++) {
        dbuf[i] = (uint8_t)get_dibit_and_analog_signal(opts, state, NULL);
    }

    for (int i = 0; i < M17_PAYLOAD_SYMBOLS; i++) {
        out_bits[i * 2 + 0] = (dbuf[i] >> 1) & 1U;
        out_bits[i * 2 + 1] = (dbuf[i] >> 0) & 1U;
    }
}

void
m17_depuncture_p2_hard(const uint8_t* punctured, uint8_t* depunc, int depunc_bits) {
    if (punctured == NULL || depunc == NULL || depunc_bits < 0) {
        return;
    }
    int bit_in = 0;
    for (int i = 0; i < depunc_bits; i++) {
        if (m17_puncture_pattern_2[i % M17_PUNCTURE_P2_LEN] == 1U && bit_in < M17_PAYLOAD_BITS) {
            depunc[i] = punctured[bit_in++];
        } else {
            depunc[i] = 0U;
        }
    }
}

void
m17_decode_bert_payload_bits(const uint8_t* m17_bits, uint8_t* bert_bits) {
    if (m17_bits == NULL || bert_bits == NULL) {
        return;
    }
    uint8_t depunc[M17_BERT_TYPE2_BITS];
    uint8_t temp[M17_BERT_TYPE2_BITS];
    uint8_t m_data[25];
    uint8_t decoded_bits[200];
    DSD_MEMSET(depunc, 0, sizeof(depunc));
    DSD_MEMSET(temp, 0, sizeof(temp));
    DSD_MEMSET(m_data, 0, sizeof(m_data));
    DSD_MEMSET(decoded_bits, 0, sizeof(decoded_bits));
    DSD_MEMSET(bert_bits, 0, M17_BERT_PAYLOAD_BITS);

    m17_depuncture_p2_hard(m17_bits, depunc, M17_BERT_TYPE2_BITS);
    for (int i = 0; i < M17_BERT_TYPE2_BITS; i++) {
        temp[i] = depunc[i] << 1U;
    }

    CNXDNConvolution_start();
    for (int i = 0; i < (M17_BERT_PAYLOAD_BITS + M17_BERT_FLUSH_BITS); i++) {
        const uint8_t s0 = temp[((size_t)2 * i)];
        const uint8_t s1 = temp[((size_t)2 * i) + 1];
        CNXDNConvolution_decode(s0, s1);
    }
    CNXDNConvolution_chainback(m_data, M17_BERT_PAYLOAD_BITS);
    unpack_byte_array_into_bit_array(m_data, decoded_bits, (int)sizeof(m_data));
    DSD_MEMCPY(bert_bits, decoded_bits, M17_BERT_PAYLOAD_BITS);
}

static void
m17_load_bert_rx_state(const dsd_state* state, m17_prbs9_rx_state* rx) {
    m17_prbs9_rx_init(rx, state->m17_bert_lfsr);
    rx->lock_count = state->m17_bert_lock_count;
    rx->window_bits = state->m17_bert_window_bits;
    rx->window_errors = state->m17_bert_window_errors;
    rx->total_bits = state->m17_bert_bits;
    rx->total_errors = state->m17_bert_errors;
    rx->resync_count = state->m17_bert_resyncs;
    rx->locked = state->m17_bert_locked;
}

static void
m17_store_bert_rx_state(dsd_state* state, const m17_prbs9_rx_state* rx) {
    state->m17_bert_lfsr = rx->lfsr;
    state->m17_bert_lock_count = rx->lock_count;
    state->m17_bert_window_bits = rx->window_bits;
    state->m17_bert_window_errors = rx->window_errors;
    state->m17_bert_bits = rx->total_bits;
    state->m17_bert_errors = rx->total_errors;
    state->m17_bert_resyncs = rx->resync_count;
    state->m17_bert_locked = rx->locked;
}

void
m17_process_bert_payload(const dsd_opts* opts, dsd_state* state, const uint8_t* bert_bits) {
    if (opts == NULL || state == NULL || bert_bits == NULL) {
        return;
    }
    m17_prbs9_rx_state rx;
    m17_load_bert_rx_state(state, &rx);

    const uint32_t prev_bits = rx.total_bits;
    const uint32_t prev_errors = rx.total_errors;
    const uint32_t prev_resyncs = rx.resync_count;
    for (int i = 0; i < M17_BERT_PAYLOAD_BITS; i++) {
        (void)m17_prbs9_rx_push_bit(&rx, bert_bits[i]);
    }
    m17_store_bert_rx_state(state, &rx);

    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, " BERT: %s bits:%u +%u errors:%u +%u resync:%u", rx.locked ? "LOCK" : "SYNCING",
                    rx.total_bits, rx.total_bits - prev_bits, rx.total_errors, rx.total_errors - prev_errors,
                    rx.resync_count - prev_resyncs);
    }
}

void
processM17BRT(dsd_opts* opts, dsd_state* state) {
    uint8_t m17_rnd_bits[M17_PAYLOAD_BITS];
    uint8_t m17_bits[M17_PAYLOAD_BITS];
    uint8_t bert_bits[M17_BERT_PAYLOAD_BITS];
    DSD_MEMSET(m17_rnd_bits, 0, sizeof(m17_rnd_bits));
    DSD_MEMSET(m17_bits, 0, sizeof(m17_bits));
    DSD_MEMSET(bert_bits, 0, sizeof(bert_bits));

    m17_read_payload_randomized_bits(opts, state, m17_rnd_bits);
    m17_payload_decode_bits(m17_rnd_bits, m17_bits);
    m17_decode_bert_payload_bits(m17_bits, bert_bits);
    m17_process_bert_payload(opts, state, bert_bits);

    DSD_FPRINTF(stderr, "\n");
}

static int
m17_finalize_lsf_crc(const dsd_opts* opts, dsd_state* state, const uint8_t* lsf_packed, uint16_t crc_ext) {
    const uint16_t crc_cmp = m17_crc16(lsf_packed, 28);
    const int crc_err = (crc_cmp != crc_ext);

    if (crc_err == 0 || opts->aggressive_framesync == 0) {
        M17decodeLSF(state);
    }

    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n LSF: ");
        for (int i = 0; i < 30; i++) {
            if (i == 15) {
                DSD_FPRINTF(stderr, "\n      ");
            }
            DSD_FPRINTF(stderr, "[%02X]", lsf_packed[i]);
        }
        DSD_FPRINTF(stderr, " (CRC CHK) E: %04X; C: %04X;", crc_ext, crc_cmp);
    }

    if (crc_err != 0) {
        DSD_FPRINTF(stderr, " CRC ERR");
    }

    return crc_err;
}

static void
m17_decode_lsf_soft_bits(const dsd_opts* opts, dsd_state* state, const uint16_t* m17_soft_bits) {
    if (!state || !m17_soft_bits) {
        return;
    }

    uint16_t m17_depunc[M17_LSF_TYPE2_BITS];
    uint8_t lsf_bytes[M17_LSF_BYTES + 1];
    uint8_t lsf_packed[M17_LSF_BYTES];

    DSD_MEMSET(m17_depunc, 0, sizeof(m17_depunc));
    DSD_MEMSET(lsf_bytes, 0, sizeof(lsf_bytes));
    DSD_MEMSET(lsf_packed, 0, sizeof(lsf_packed));

    m17_soft_depuncture_p1(m17_soft_bits, m17_depunc);
    (void)viterbi_decode(lsf_bytes, m17_depunc, M17_LSF_TYPE2_BITS);
    DSD_MEMCPY(lsf_packed, lsf_bytes + 1, M17_LSF_BYTES);

    DSD_MEMSET(state->m17_lsf, 0, sizeof(state->m17_lsf));
    unpack_byte_array_into_bit_array(lsf_packed, state->m17_lsf, M17_LSF_BYTES);

    const uint16_t crc_ext = (uint16_t)((lsf_packed[M17_LSF_LSD_BYTES] << 8) + lsf_packed[M17_LSF_LSD_BYTES + 1]);
    (void)m17_finalize_lsf_crc(opts, state, lsf_packed, crc_ext);
}

// Decode an RF LSF using captured soft symbols.
void
processM17LSF(dsd_opts* opts, dsd_state* state) {
    float soft_symbols[M17_PAYLOAD_SYMBOLS];
    uint16_t m17_soft_bits[M17_PAYLOAD_BITS];

    DSD_MEMSET(soft_symbols, 0, sizeof(soft_symbols));
    DSD_MEMSET(m17_soft_bits, 0, sizeof(m17_soft_bits));

    m17_capture_soft_symbols(opts, state, soft_symbols);
    m17_soft_bits_from_symbols(soft_symbols, state, m17_soft_bits);
    m17_decode_lsf_soft_bits(opts, state, m17_soft_bits);

    DSD_FPRINTF(stderr, "\n");
} //end processM17LSF

static void
m17_monitor_encoded_lsf(const dsd_opts* opts, dsd_state* state, const uint8_t* m17_rnd_bits) {
    uint8_t m17_bits[M17_PAYLOAD_BITS];
    uint16_t m17_soft_bits[M17_PAYLOAD_BITS];

    DSD_MEMSET(m17_bits, 0, sizeof(m17_bits));
    DSD_MEMSET(m17_soft_bits, 0, sizeof(m17_soft_bits));

    m17_payload_decode_bits(m17_rnd_bits, m17_bits);
    for (size_t i = 0U; i < M17_PAYLOAD_BITS; i++) {
        m17_soft_bits[i] = m17_bits[i] ? 0xFFFFU : 0U;
    }
    m17_decode_lsf_soft_bits(opts, state, m17_soft_bits);
}

/* Decode the transmitted frame through the receive path for encoded-audio monitoring and state reporting. */
static void
m17_monitor_encoded_stream(const dsd_opts* opts, dsd_state* state, const uint8_t* m17_rnd_bits) {
    uint8_t m17_bits[M17_PAYLOAD_BITS];
    DSD_MEMSET(m17_bits, 0, sizeof(m17_bits));

    m17_payload_decode_bits(m17_rnd_bits, m17_bits);
    if (m17_process_lich(state, opts, m17_bits) == 0) {
        M17prepareStream(opts, state, m17_bits);
    }
}

_Static_assert(M17_RRC_RECOMMENDED_TAPS == DSD_M17_RRC_48KHZ_TAP_COUNT, "M17 RRC tap count mismatch");

static float mem[M17_RRC_RECOMMENDED_TAPS];

void
m17_dibits_to_symbols(const uint8_t* output_dibits, int* output_symbols) {
    if (output_dibits == NULL || output_symbols == NULL) {
        return;
    }
    for (int i = 0; i < M17_FRAME_SYMBOLS; i++) {
        output_symbols[i] = m17_symbol_from_dibit(output_dibits[i]);
    }
}

void
m17_upsample_symbols_10x(const int* output_symbols, int* output_up) {
    if (output_symbols == NULL || output_up == NULL) {
        return;
    }
    for (int i = 0; i < M17_FRAME_SYMBOLS; i++) {
        for (int j = 0; j < M17_RECOMMENDED_UPSAMPLE_FACTOR; j++) {
            output_up[((size_t)i * M17_RECOMMENDED_UPSAMPLE_FACTOR) + j] = output_symbols[i];
        }
    }
}

void
m17_baseband_no_filter(const int* output_up, short* baseband) {
    if (output_up == NULL || baseband == NULL) {
        return;
    }
    for (size_t i = 0U; i < M17_BASEBAND_SAMPLES; i++) {
        baseband[i] = (short)(output_up[i] * 7168.0f);
    }
}

static void
m17_baseband_rrc_filter(const int* output_symbols, short* baseband) {
    int out = 0;
    for (int i = 0; i < M17_FRAME_SYMBOLS; i++) {
        mem[0] = (float)output_symbols[i] * 7168.0f;
        for (int j = 0; j < M17_RECOMMENDED_UPSAMPLE_FACTOR; j++) {
            float mac = 0.0f;
            for (int k = 0; k < M17_RRC_RECOMMENDED_TAPS; k++) {
                mac += mem[k] * dsd_m17_rrc_48khz_taps[k] * sqrtf((float)M17_RECOMMENDED_UPSAMPLE_FACTOR);
            }
            for (int k = M17_RRC_RECOMMENDED_TAPS - 1; k > 0; k--) {
                mem[k] = mem[k - 1];
            }
            mem[0] = 0.0f;
            baseband[out++] = (short)mac;
        }
    }
}

static void
m17_build_baseband(const dsd_opts* opts, const int* output_symbols, const int* output_up, short* baseband) {
    if (opts->use_cosine_filter == 1) {
        m17_baseband_rrc_filter(output_symbols, baseband);
    } else {
        m17_baseband_no_filter(output_up, baseband);
    }
}

void
m17_maybe_apply_dead_air(int type, uint8_t* output_dibits, short* baseband) {
    if (output_dibits == NULL || baseband == NULL) {
        return;
    }
    if (type == 99) {
        DSD_MEMSET(output_dibits, 0xFF, M17_FRAME_SYMBOLS);
        DSD_MEMSET(baseband, 0, M17_BASEBAND_BYTES);
    }
}

static void
m17_write_symbol_capture_if_enabled(dsd_opts* opts, dsd_state* state, const uint8_t* output_dibits,
                                    const int* output_symbols) {
    if (!opts->symbol_out_f) {
        return;
    }
    for (int i = 0; i < M17_FRAME_SYMBOLS; i++) {
        write_symbol_capture_record(opts, state, output_dibits[i], (float)output_symbols[i], NULL);
    }
}

static void
m17_write_dsp_symbols_if_enabled(const dsd_opts* opts, const int* output_symbols) {
    if (!opts->use_dsp_output) {
        return;
    }

    FILE* pfile = dsd_fopen_private(opts->dsp_out_file, "a");
    if (pfile == NULL) {
        return;
    }

    for (int i = 0; i < M17_FRAME_SYMBOLS; i++) {
        const float val = (float)output_symbols[i];
        fwrite(&val, 4, 1, pfile);
    }
    fclose(pfile);
}

static void
m17_monitor_baseband_if_enabled(dsd_opts* opts, dsd_state* state, const short* baseband) {
    if (!(opts->monitor_input_audio == 1 && opts->audio_out == 1)) {
        return;
    }

    if (opts->audio_out_type == 0) {
        dsd_audio_write(opts->audio_raw_out, baseband, M17_BASEBAND_SAMPLES);
        return;
    }

    if (opts->audio_out_type == 8) {
        dsd_udp_audio_hook_blast_analog(opts, state, M17_BASEBAND_BYTES, (short*)baseband);
        return;
    }

    if (opts->audio_out_type == 1) {
        const ssize_t written = dsd_write(opts->audio_out_fd, baseband, M17_BASEBAND_BYTES);
        if (written < 0) {
            LOG_WARN("encodeM17RF: failed to write %zu-byte baseband block", M17_BASEBAND_BYTES);
        }
    }
}

static void
m17_write_baseband_wav_if_enabled(const dsd_opts* opts, const short* baseband) {
    if (opts->wav_out_raw != NULL) {
        m17_write_wav_short_block(opts->wav_out_raw, baseband, (sf_count_t)M17_BASEBAND_SAMPLES, "M17 raw WAV");
        sf_write_sync(opts->wav_out_raw);
    }
}

void
m17_reverse_brt_bits(const uint8_t* input, uint8_t* output) {
    if (input == NULL || output == NULL) {
        return;
    }
    DSD_MEMSET(output, 0, 208);
    for (int i = 0; i < M17_BERT_PAYLOAD_BITS; i++) {
        output[i + 3] = input[(M17_BERT_PAYLOAD_BITS - 1) - i];
    }
}

static void
m17_print_brt_sequence(const uint8_t* m17_b1) {
    uint8_t m17_b1r[208];
    m17_reverse_brt_bits(m17_b1, m17_b1r);
    DSD_FPRINTF(stderr, "\n M17 BERT   (ENCODER): ");
    for (int i = 0; i < 25; i++) {
        DSD_FPRINTF(stderr, "%02X", (uint8_t)convert_bits_into_output(&m17_b1r[((size_t)i * 8)], 8));
    }
}

//convert bit array into symbols and RF/Audio
static void
encodeM17RF(dsd_opts* opts, dsd_state* state, const uint8_t* input, int type) {
    uint8_t output_dibits[M17_FRAME_SYMBOLS];
    DSD_MEMSET(output_dibits, 0, sizeof(output_dibits));
    switch (type) {
        case 1: m17_frame_build_dibits(M17_SYNC_LSF_WORD, input, output_dibits); break;
        case 2: m17_frame_build_dibits(M17_SYNC_STREAM_WORD, input, output_dibits); break;
        case 3: m17_frame_build_dibits(M17_SYNC_BERT_WORD, input, output_dibits); break;
        case 4: m17_frame_build_dibits(M17_SYNC_PACKET_WORD, input, output_dibits); break;
        case 11: m17_fill_repeating_16bit_dibits(M17_PREAMBLE_LSF_WORD, output_dibits); break;
        case 33: m17_fill_repeating_16bit_dibits(M17_PREAMBLE_BERT_WORD, output_dibits); break;
        case 55: m17_fill_repeating_16bit_dibits(M17_EOT_MARKER_WORD, output_dibits); break;
        default: break;
    }

    int output_symbols[M17_FRAME_SYMBOLS];
    DSD_MEMSET(output_symbols, 0, sizeof(output_symbols));
    m17_dibits_to_symbols(output_dibits, output_symbols);

    int output_up[M17_FRAME_SYMBOLS * M17_RECOMMENDED_UPSAMPLE_FACTOR];
    DSD_MEMSET(output_up, 0, sizeof(output_up));
    m17_upsample_symbols_10x(output_symbols, output_up);

    short baseband[M17_FRAME_SYMBOLS * M17_RECOMMENDED_UPSAMPLE_FACTOR];
    DSD_MEMSET(baseband, 0, sizeof(baseband));
    m17_build_baseband(opts, output_symbols, output_up, baseband);
    m17_maybe_apply_dead_air(type, output_dibits, baseband);
    m17_write_symbol_capture_if_enabled(opts, state, output_dibits, output_symbols);
    m17_write_dsp_symbols_if_enabled(opts, output_symbols);
    m17_monitor_baseband_if_enabled(opts, state, baseband);
    m17_write_baseband_wav_if_enabled(opts, baseband);

    //NOTE: Internal voice decoding is disabled when tx audio over a hardware device, wav/bin still enabled
    UNUSED(state);
}

static const uint8_t m17_ip_magic_stream[4] = {0x4D, 0x31, 0x37, 0x20};
static const uint8_t m17_ip_magic_mpkt[4] = {0x4D, 0x50, 0x4B, 0x54};

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
    uint8_t st;
    uint8_t can;
    int use_ip;
    int udpport;
    unsigned long long int dst;
    unsigned long long int src;
    char d40[50];
    char s40[50];
    uint8_t nil[368];
    float sample;
    size_t nsam;
    int dec;
    int sql_hit;
    int eot_out;
    short* samp1;
    short* samp2;
    short* voice1;
    short* voice2;
    uint16_t fsn;
    uint8_t eot;
    uint8_t lich_cnt;
    uint8_t lsf_chunk[6][48];
    uint8_t m17_lsf[240];
    uint8_t lsf_packed[30];
    uint16_t crc_cmp;
    uint8_t m17_lsfs[368];
    uint8_t conn[11];
    uint8_t disc[10];
    uint8_t eotx[10];
    uint8_t sid[2];
    uint8_t m17_ip_frame[432];
    uint8_t m17_ip_packed[54];
    int new_lsf;
} m17_str_ctx;

typedef struct {
    uint8_t vc1_bytes[8];
    uint8_t vc2_bytes[8];
    uint8_t v1_bits[64];
    uint8_t v2_bits[64];
    uint8_t m17_t4s[368];
} m17_str_frame_ctx;

static void
m17_send_dead_air_frames(dsd_opts* opts, dsd_state* state, uint8_t* nil, int count) {
    DSD_MEMSET(nil, 0, 368);
    for (int i = 0; i < count; i++) {
        encodeM17RF(opts, state, nil, 99);
    }
}

static int
m17_open_udp_if_enabled(dsd_opts* opts, dsd_state* state) {
    if (opts->m17_use_ip != 1) {
        return 0;
    }
    const int sock_err = dsd_m17_udp_hook_connect(opts, state);
    if (sock_err != 0) {
        DSD_FPRINTF(stderr, "Error Configuring UDP Socket for M17 IP Frame :( \n");
        return -1;
    }
    return 1;
}

void
m17_setup_conn_disc_eotx(unsigned long long int src, uint8_t reflector_module, uint8_t* conn, uint8_t* disc,
                         uint8_t* eotx) {
    if (conn == NULL || disc == NULL || eotx == NULL) {
        return;
    }
    DSD_MEMSET(conn, 0, 11);
    DSD_MEMSET(disc, 0, 10);
    DSD_MEMSET(eotx, 0, 10);

    conn[0] = 0x43;
    conn[1] = 0x4F;
    conn[2] = 0x4E;
    conn[3] = 0x4E;
    conn[10] = reflector_module;

    disc[0] = 0x44;
    disc[1] = 0x49;
    disc[2] = 0x53;
    disc[3] = 0x43;

    eotx[0] = 0x45;
    eotx[1] = 0x4F;
    eotx[2] = 0x54;
    eotx[3] = 0x58;

    conn[4] = (src >> 40ULL) & 0xFF;
    conn[5] = (src >> 32ULL) & 0xFF;
    conn[6] = (src >> 24ULL) & 0xFF;
    conn[7] = (src >> 16ULL) & 0xFF;
    conn[8] = (src >> 8ULL) & 0xFF;
    conn[9] = (src >> 0ULL) & 0xFF;
    for (int i = 0; i < 6; i++) {
        disc[i + 4] = conn[i + 4];
        eotx[i + 4] = conn[i + 4];
    }
}

void
m17_load_lsf_callsigns(uint8_t* m17_lsf, unsigned long long int dst, unsigned long long int src) {
    if (m17_lsf == NULL) {
        return;
    }
    for (int i = 0; i < 48; i++) {
        m17_lsf[i] = (dst >> (47ULL - (unsigned long long int)i)) & 1;
    }
    for (int i = 0; i < 48; i++) {
        m17_lsf[i + 48] = (src >> (47ULL - (unsigned long long int)i)) & 1;
    }
}

uint16_t
m17_attach_lsf_crc(uint8_t* m17_lsf, uint8_t* lsf_packed) {
    if (m17_lsf == NULL || lsf_packed == NULL) {
        return 0U;
    }
    DSD_MEMSET(lsf_packed, 0, 30);
    for (int i = 0; i < 28; i++) {
        lsf_packed[i] = (uint8_t)convert_bits_into_output(&m17_lsf[((size_t)i * 8)], 8);
    }
    const uint16_t crc_cmp = m17_crc16(lsf_packed, 28);
    for (int i = 0; i < 16; i++) {
        m17_lsf[224 + i] = (crc_cmp >> (15 - i)) & 1;
    }
    return crc_cmp;
}

static void
m17_encode_lsf_for_rf(const uint8_t m17_lsf[M17_LSF_TYPE1_BITS], uint8_t m17_lsfs[M17_PAYLOAD_BITS]) {
    uint8_t type1_flush_bits[M17_LSF_TYPE1_FLUSH_BITS];
    DSD_MEMSET(type1_flush_bits, 0, sizeof(type1_flush_bits));
    DSD_MEMCPY(type1_flush_bits, m17_lsf, M17_LSF_TYPE1_BITS);
    (void)m17_lsf_encode_type1_bits(type1_flush_bits, m17_lsfs, NULL);
}

static float
m17_scale_input_sample(float sample, int multiplier) {
    if (multiplier > 1) {
        int v = (int)sample * multiplier;
        if (v > 32767) {
            v = 32767;
        } else if (v < -32768) {
            v = -32768;
        }
        sample = (float)v;
    }
    return sample;
}

enum m17_str_read_result {
    M17_STR_READ_ERROR = -1,
    M17_STR_READ_STOP = 0,
    M17_STR_READ_OK = 1,
};

static int
m17_str_read_block_pulse(dsd_opts* opts, size_t nsam, int dec, float* sample, short* out, int clip_output) {
    for (size_t i = 0; i < nsam; i++) {
        for (int j = 0; j < dec; j++) {
            short s = 0;
            dsd_audio_read(opts->audio_in_stream, &s, 1);
            *sample = (float)s;
        }
        *sample = m17_scale_input_sample(*sample, opts->input_volume_multiplier);
        out[i] = clip_output ? m17_clip_float_to_short(*sample) : (short)*sample;
    }
    return M17_STR_READ_OK;
}

static int
m17_str_read_block_stdin(dsd_opts* opts, size_t nsam, int dec, float* sample, short* out) {
    if (opts->audio_in_file == NULL) {
        LOG_ERROR("M17 stream encoder has no STDIN audio handle");
        return M17_STR_READ_ERROR;
    }

    for (size_t i = 0; i < nsam; i++) {
        for (int j = 0; j < dec; j++) {
            short s = 0;
            const sf_count_t result = sf_read_short(opts->audio_in_file, &s, 1);
            if (result != 1) {
                const int read_error = sf_error(opts->audio_in_file);
                if (read_error != SF_ERR_NO_ERROR) {
                    LOG_ERROR("M17 stream encoder failed to read STDIN: %s", sf_strerror(opts->audio_in_file));
                }
                sf_close(opts->audio_in_file);
                opts->audio_in_file = NULL;
                exitflag = 1;
                if (read_error != SF_ERR_NO_ERROR) {
                    return M17_STR_READ_ERROR;
                }
                DSD_FPRINTF(stderr, "Connection to STDIN Disconnected.\n");
                DSD_FPRINTF(stderr, "Closing DSD-neo.\n");
                return M17_STR_READ_STOP;
            }
            *sample = (float)s;
        }
        *sample = m17_scale_input_sample(*sample, opts->input_volume_multiplier);
        out[i] = m17_clip_float_to_short(*sample);
    }
    return M17_STR_READ_OK;
}

static int
m17_str_read_block_tcp(dsd_opts* opts, size_t nsam, int dec, float* sample, short* out) {
    for (size_t i = 0; i < nsam; i++) {
        for (int j = 0; j < dec; j++) {
            short s = 0;
            const int result = dsd_net_audio_input_hook_tcp_read_sample(opts->tcp_in_ctx, (int16_t*)&s);
            if (result == 0) {
                dsd_net_audio_input_hook_tcp_close(opts->tcp_in_ctx);
                opts->tcp_in_ctx = NULL;
                DSD_FPRINTF(stderr, "Connection to TCP Server Disconnected.\n");
                DSD_FPRINTF(stderr, "Closing DSD-neo.\n");
                exitflag = 1;
                return M17_STR_READ_STOP;
            }
            *sample = (float)s;
        }
        *sample = m17_scale_input_sample(*sample, opts->input_volume_multiplier);
        out[i] = m17_clip_float_to_short(*sample);
    }
    return M17_STR_READ_OK;
}

static int
m17_str_read_block_udp(dsd_opts* opts, size_t nsam, int dec, float* sample, short* out, int clip_output) {
    for (size_t i = 0; i < nsam; i++) {
        for (int j = 0; j < dec; j++) {
            short s = 0;
            if (!dsd_net_audio_input_hook_udp_read_sample(opts, (int16_t*)&s)) {
                DSD_FPRINTF(stderr, "UDP input stopped.\n");
                exitflag = 1;
                return M17_STR_READ_STOP;
            }
            *sample = (float)s;
        }
        *sample = m17_scale_input_sample(*sample, opts->input_volume_multiplier);
        out[i] = clip_output ? m17_clip_float_to_short(*sample) : (short)*sample;
    }
    return M17_STR_READ_OK;
}

static int
m17_str_read_block_rtl(dsd_opts* opts, dsd_state* state, size_t nsam, int dec, float* sample, short* out,
                       int clip_output) {
#ifdef USE_RADIO
    for (size_t i = 0; i < nsam; i++) {
        for (int j = 0; j < dec; j++) {
            if (!state->rtl_ctx) {
                dsd_request_shutdown(opts, state);
                return M17_STR_READ_STOP;
            }
            int got = 0;
            if (dsd_rtl_stream_io_hook_read(state, sample, 1, &got) < 0 || got != 1) {
                dsd_request_shutdown(opts, state);
                return M17_STR_READ_STOP;
            }
        }
        *sample *= opts->rtl_volume_multiplier;
        out[i] = clip_output ? m17_clip_float_to_short(*sample) : (short)*sample;
    }
    opts->rtl_pwr = dsd_rtl_stream_io_hook_return_pwr(state);
    return M17_STR_READ_OK;
#else
    UNUSED(opts);
    UNUSED(state);
    UNUSED(nsam);
    UNUSED(dec);
    UNUSED(sample);
    UNUSED(out);
    UNUSED(clip_output);
    return M17_STR_READ_ERROR;
#endif
}

static int
m17_str_read_audio_block(m17_str_ctx* ctx, short* out, int clip_output) {
    switch (ctx->opts->audio_in_type) {
        case AUDIO_IN_PULSE:
            return m17_str_read_block_pulse(ctx->opts, ctx->nsam, ctx->dec, &ctx->sample, out, clip_output);
        case AUDIO_IN_STDIN: return m17_str_read_block_stdin(ctx->opts, ctx->nsam, ctx->dec, &ctx->sample, out);
        case AUDIO_IN_TCP: return m17_str_read_block_tcp(ctx->opts, ctx->nsam, ctx->dec, &ctx->sample, out);
        case AUDIO_IN_UDP:
            return m17_str_read_block_udp(ctx->opts, ctx->nsam, ctx->dec, &ctx->sample, out, clip_output);
        case AUDIO_IN_RTL:
            return m17_str_read_block_rtl(ctx->opts, ctx->state, ctx->nsam, ctx->dec, &ctx->sample, out, clip_output);
        default: return M17_STR_READ_ERROR;
    }
}

static int
m17_str_read_audio_inputs(m17_str_ctx* ctx) {
    int result = m17_str_read_audio_block(ctx, ctx->voice1, 1);
    if (result != M17_STR_READ_OK) {
        return result;
    }

    if (ctx->st == 2) {
        const int clip_output = !(ctx->opts->audio_in_type == AUDIO_IN_UDP || ctx->opts->audio_in_type == AUDIO_IN_RTL);
        result = m17_str_read_audio_block(ctx, ctx->voice2, clip_output);
        if (result != M17_STR_READ_OK) {
            return result;
        }
    }
    return M17_STR_READ_OK;
}

static void
m17_str_apply_monitor_side_state(m17_str_ctx* ctx) {
    if (ctx->opts->monitor_input_audio != 1) {
        return;
    }
    DSD_SNPRINTF(ctx->state->m17_src_str, sizeof(ctx->state->m17_src_str), "%s", ctx->s40);
    DSD_SNPRINTF(ctx->state->m17_dst_str, sizeof(ctx->state->m17_dst_str), "%s", ctx->d40);
    ctx->state->m17_src = ctx->src;
    ctx->state->m17_dst = ctx->dst;
    ctx->state->m17_can = ctx->can;
    ctx->state->m17_str_dt = ctx->st;
    ctx->state->m17_enc = 0;
    ctx->state->m17_enc_st = 0;
    ctx->state->m17_payload_decrypted = 0;
    ctx->state->m17_signature_advertised = 0;
    DSD_MEMSET(ctx->state->m17_signature_digest, 0, sizeof(ctx->state->m17_signature_digest));
    DSD_MEMSET(ctx->state->m17_signature, 0, sizeof(ctx->state->m17_signature));
    ctx->state->m17_signature_received_mask = 0;
    ctx->state->m17_signature_complete = 0;
    ctx->state->m17_signature_bad_sequence = 0;
    ctx->state->m17_signature_verification_status = M17_SIGNATURE_VERIFY_NOT_RUN;
    for (int i = 0; i < 16; i++) {
        ctx->state->m17_meta[i] = 0;
    }
}

static void
m17_str_update_input_power(m17_str_ctx* ctx) {
    dsd_input_level_snapshot snapshot;
    if (ctx->opts->audio_in_type == AUDIO_IN_RTL) {
        if (dsd_rtl_stream_metrics_hook_input_level(&snapshot) == 0 && snapshot.sample_count > 0U) {
            dsd_input_level_publish(ctx->opts, ctx->state, &snapshot, DSD_INPUT_LEVEL_NOTIFY_RF);
        }
        return;
    }
    if (dsd_input_level_metrics_from_pcm_i16(ctx->voice1, (size_t)ctx->nsam, 1U, DSD_INPUT_LEVEL_SOURCE_PCM, &snapshot)
        == 0) {
        dsd_input_level_publish(ctx->opts, ctx->state, &snapshot, DSD_INPUT_LEVEL_NOTIFY_ALL);
    }
}

static void
m17_str_apply_filters_and_gain(m17_str_ctx* ctx) {
    if (ctx->opts->use_lpf == 1) {
        lpf(ctx->state, ctx->voice1, 160);
        if (ctx->st == 2) {
            lpf(ctx->state, ctx->voice2, 160);
        }
    }
    if (ctx->opts->use_hpf == 1) {
        hpf(ctx->state, ctx->voice1, 160);
        if (ctx->st == 2) {
            hpf(ctx->state, ctx->voice2, 160);
        }
    }
    if (ctx->opts->use_pbf == 1) {
        pbf(ctx->state, ctx->voice1, 160);
        if (ctx->st == 2) {
            pbf(ctx->state, ctx->voice2, 160);
        }
    }

    if (ctx->opts->audio_gainA > 0.0f) {
        analog_gain(ctx->opts, ctx->state, ctx->voice1, 160);
        if (ctx->st == 2) {
            analog_gain(ctx->opts, ctx->state, ctx->voice2, 160);
        }
    } else {
        agsm(ctx->opts, ctx->state, ctx->voice1, 160);
        if (ctx->st == 2) {
            agsm(ctx->opts, ctx->state, ctx->voice2, 160);
        }
    }
}

static void
m17_str_encode_codec2(const m17_str_ctx* ctx, m17_str_frame_ctx* frame) {
    DSD_MEMSET(frame->vc1_bytes, 0, sizeof(frame->vc1_bytes));
    DSD_MEMSET(frame->vc2_bytes, 0, sizeof(frame->vc2_bytes));

#ifdef USE_CODEC2
    if (ctx->st == 2) {
        codec2_encode(ctx->state->codec2_3200, frame->vc1_bytes, ctx->voice1);
        codec2_encode(ctx->state->codec2_3200, frame->vc2_bytes, ctx->voice2);
    } else if (ctx->st == 3) {
        codec2_encode(ctx->state->codec2_1600, frame->vc1_bytes, ctx->voice1);
    }
#endif

    if (ctx->st == 3) {
        DSD_MEMCPY(frame->vc2_bytes, ctx->state->m17sms + ((size_t)ctx->lich_cnt * 8), 8);
    }
}

static void
m17_str_pack_voice_bits(m17_str_frame_ctx* frame) {
    int k = 0;
    int x = 0;
    DSD_MEMSET(frame->v1_bits, 0, sizeof(frame->v1_bits));
    DSD_MEMSET(frame->v2_bits, 0, sizeof(frame->v2_bits));
    for (int j = 0; j < 8; j++) {
        for (int i = 0; i < 8; i++) {
            frame->v1_bits[k++] = (frame->vc1_bytes[j] >> (7 - i)) & 1;
            frame->v2_bits[x++] = (frame->vc2_bytes[j] >> (7 - i)) & 1;
        }
    }
}

static void
m17_str_update_vox_and_eot(m17_str_ctx* ctx) {
    if (ctx->opts->rtl_pwr > ctx->opts->rtl_squelch_level) {
        ctx->sql_hit = 0;
    } else {
        ctx->sql_hit++;
    }

    if (ctx->state->m17_vox == 1) {
        if (ctx->sql_hit > 10 && ctx->lich_cnt == 0) {
            ctx->state->m17encoder_tx = 0;
        } else {
            ctx->state->m17encoder_tx = 1;
            ctx->eot = 0;
        }
    }

    if (exitflag) {
        ctx->eot = 1;
    }
    if (ctx->state->m17encoder_eot) {
        ctx->eot = 1;
    }
}

static void
m17_str_build_stream_frame(m17_str_ctx* ctx, m17_str_frame_ctx* frame) {
    uint8_t m17_v1[M17_STREAM_TYPE1_FLUSH_BITS];
    uint8_t m17_v1p[M17_STREAM_PUNCTURED_BITS];
    uint8_t m17_l1g[M17_LICH_BITS];
    uint8_t m17_t4c[M17_PAYLOAD_BITS];
    DSD_MEMSET(m17_v1, 0, sizeof(m17_v1));
    DSD_MEMSET(m17_v1p, 0, sizeof(m17_v1p));
    DSD_MEMSET(m17_l1g, 0, sizeof(m17_l1g));
    DSD_MEMSET(m17_t4c, 0, sizeof(m17_t4c));
    DSD_MEMSET(frame->m17_t4s, 0, sizeof(frame->m17_t4s));

    m17_str_pack_voice_bits(frame);
    m17_str_update_vox_and_eot(ctx);
    uint8_t payload[M17_STREAM_PAYLOAD_BITS];
    m17_stream_pack_payload_halves(frame->v1_bits, frame->v2_bits, payload);
    const uint16_t frame_number = (uint16_t)((ctx->eot != 0U ? M17_STREAM_FRAME_END_MASK : 0U) | ctx->fsn);
    m17_stream_build_type1_bits(frame_number, payload, m17_v1);

    m17_stream_encode_type1_bits(m17_v1, m17_v1p);

    (void)m17_lich_build_content(ctx->m17_lsf, ctx->lich_cnt, ctx->lsf_chunk[ctx->lich_cnt]);
    m17_lich_encode_bits(ctx->lsf_chunk[ctx->lich_cnt], m17_l1g);
    m17_stream_combine_frame_bits(m17_l1g, m17_v1p, m17_t4c);
    m17_payload_encode_bits(m17_t4c, frame->m17_t4s);
}

static void
m17_str_build_ip_frame(m17_str_ctx* ctx, const m17_str_frame_ctx* frame) {
    DSD_MEMSET(ctx->m17_ip_frame, 0, sizeof(ctx->m17_ip_frame));
    DSD_MEMSET(ctx->m17_ip_packed, 0, sizeof(ctx->m17_ip_packed));

    int k = 0;
    for (int j = 0; j < 4; j++) {
        for (int i = 0; i < 8; i++) {
            ctx->m17_ip_frame[k++] = (m17_ip_magic_stream[j] >> (7 - i)) & 1;
        }
    }
    for (int j = 0; j < 2; j++) {
        for (int i = 0; i < 8; i++) {
            ctx->m17_ip_frame[k++] = (ctx->sid[j] >> (7 - i)) & 1;
        }
    }
    for (int i = 0; i < 224; i++) {
        ctx->m17_ip_frame[k++] = ctx->m17_lsf[i];
    }

    ctx->m17_ip_frame[k++] = ctx->eot & 1;
    for (int i = 0; i < 15; i++) {
        ctx->m17_ip_frame[k++] = (ctx->fsn >> (14 - i)) & 1;
    }
    for (int i = 0; i < 64; i++) {
        ctx->m17_ip_frame[k++] = frame->v1_bits[i];
    }
    for (int i = 0; i < 64; i++) {
        ctx->m17_ip_frame[k++] = frame->v2_bits[i];
    }

    for (int i = 0; i < 52; i++) {
        ctx->m17_ip_packed[i] = (uint8_t)convert_bits_into_output(&ctx->m17_ip_frame[((size_t)i * 8)], 8);
    }
    const uint16_t ip_crc = m17_crc16(ctx->m17_ip_packed, 52);
    for (int i = 0; i < 16; i++) {
        ctx->m17_ip_frame[k++] = (ip_crc >> (15 - i)) & 1;
    }
    for (int i = 52; i < 54; i++) {
        ctx->m17_ip_packed[i] = (uint8_t)convert_bits_into_output(&ctx->m17_ip_frame[((size_t)i * 8)], 8);
    }
}

static void
m17_str_send_lsf_if_needed(m17_str_ctx* ctx) {
    if (ctx->new_lsf != 1) {
        return;
    }

    DSD_FPRINTF(stderr, "\n M17 LSF    (ENCODER): ");
    if (ctx->opts->monitor_input_audio == 0) {
        m17_monitor_encoded_lsf(ctx->opts, ctx->state, ctx->m17_lsfs);
    } else {
        DSD_FPRINTF(stderr, " To Audio Out Device Type: %d; ", ctx->opts->audio_out_type);
    }

    DSD_MEMSET(ctx->nil, 0, sizeof(ctx->nil));
    encodeM17RF(ctx->opts, ctx->state, ctx->nil, 11);
    encodeM17RF(ctx->opts, ctx->state, ctx->m17_lsfs, 1);

    ctx->new_lsf = 0;
    ctx->eot_out = 0;
}

static void
m17_str_report_encoded_stream(const m17_str_ctx* ctx, const m17_str_frame_ctx* frame, int udp_gate_on_lich5) {
    DSD_FPRINTF(stderr, "\n M17 Stream (ENCODER): ");
    if (ctx->opts->monitor_input_audio == 0) {
        m17_monitor_encoded_stream(ctx->opts, ctx->state, frame->m17_t4s);
    } else {
        DSD_FPRINTF(stderr, " To Audio Out Device Type: %d; ", ctx->opts->audio_out_type);
    }

    if (ctx->use_ip == 1 && (!udp_gate_on_lich5 || ctx->lich_cnt != 5)) {
        DSD_FPRINTF(stderr, " UDP: %s:%d", ctx->opts->m17_hostname, ctx->udpport);
    }
    if (ctx->state->m17_vox == 1) {
        DSD_FPRINTF(stderr, " PWR: %.1f dB", pwr_to_dB(ctx->opts->rtl_pwr));
        DSD_FPRINTF(stderr, " SQL HIT: %d;", ctx->sql_hit);
    }
}

static void
m17_str_handle_tx_active(m17_str_ctx* ctx, const m17_str_frame_ctx* frame) {
    ctx->state->carrier = 1;
    ctx->state->synctype = DSD_SYNC_M17_STR_POS;

    m17_str_send_lsf_if_needed(ctx);
    m17_str_report_encoded_stream(ctx, frame, 1);
    encodeM17RF(ctx->opts, ctx->state, frame->m17_t4s, 2);

    m17_str_build_ip_frame(ctx, frame);
    if (ctx->use_ip == 1) {
        (void)dsd_m17_udp_hook_blaster(ctx->opts, ctx->state, 54, ctx->m17_ip_packed);
    }

    ctx->lich_cnt++;
    if (ctx->lich_cnt == 6) {
        ctx->lich_cnt = 0;
    }
    ctx->fsn = m17_stream_next_frame_counter(ctx->fsn);
}

static void
m17_str_reset_tx_idle_state(m17_str_ctx* ctx) {
    ctx->lich_cnt = 0;
    ctx->fsn = 0;
    ctx->state->carrier = 0;
    ctx->state->synctype = DSD_SYNC_NONE;

    dsd_nonce_fill(ctx->sid, sizeof(ctx->sid));

    ctx->crc_cmp = m17_attach_lsf_crc(ctx->m17_lsf, ctx->lsf_packed);
    m17_encode_lsf_for_rf(ctx->m17_lsf, ctx->m17_lsfs);
}

static void
m17_str_flush_eot_if_needed(m17_str_ctx* ctx, const m17_str_frame_ctx* frame) {
    if (!(ctx->eot && !ctx->eot_out)) {
        return;
    }

    m17_str_report_encoded_stream(ctx, frame, 0);
    encodeM17RF(ctx->opts, ctx->state, frame->m17_t4s, 2);

    DSD_MEMSET(ctx->nil, 0, sizeof(ctx->nil));
    encodeM17RF(ctx->opts, ctx->state, ctx->nil, 55);
    m17_send_dead_air_frames(ctx->opts, ctx->state, ctx->nil, 25);

    if (ctx->use_ip == 1) {
        (void)dsd_m17_udp_hook_blaster(ctx->opts, ctx->state, 54, ctx->m17_ip_packed);
        (void)dsd_m17_udp_hook_blaster(ctx->opts, ctx->state, 10, ctx->eotx);
    }

    ctx->eot = 0;
    ctx->eot_out = 1;
    ctx->state->m17encoder_eot = 0;
}

static void
m17_str_handle_tx_idle(m17_str_ctx* ctx, const m17_str_frame_ctx* frame) {
    m17_str_build_ip_frame(ctx, frame);
    m17_str_reset_tx_idle_state(ctx);
    m17_str_flush_eot_if_needed(ctx, frame);
    ctx->new_lsf = 1;
    DSD_MEMSET(ctx->state->m17_meta, 0, sizeof(ctx->state->m17_meta));
    DSD_MEMSET(ctx->state->m17_lsf, 0, sizeof(ctx->state->m17_lsf));
}

static void
m17_str_iter_tail(m17_str_ctx* ctx) {
    if (dsd_opts_frontend_active(ctx->opts)) {
        dsd_telemetry_publish_both_and_redraw(ctx->opts, ctx->state);
    }
    watchdog_event_history(ctx->opts, ctx->state, 0);
    watchdog_event_current(ctx->opts, ctx->state, 0);
}

static int
m17_str_prepare_audio_buffers(m17_str_ctx* ctx) {
    ctx->samp1 = malloc(sizeof(short) * ctx->nsam);
    ctx->samp2 = malloc(sizeof(short) * ctx->nsam);
    ctx->voice1 = malloc(sizeof(short) * ctx->nsam);
    ctx->voice2 = malloc(sizeof(short) * ctx->nsam);
    if (!ctx->samp1 || !ctx->samp2 || !ctx->voice1 || !ctx->voice2) {
        DSD_FPRINTF(stderr, "encodeM17STR: out of memory allocating %zu-sample buffers\n", ctx->nsam);
        free(ctx->samp1);
        free(ctx->samp2);
        free(ctx->voice1);
        free(ctx->voice2);
        return 0;
    }
    return 1;
}

static void
m17_str_prepare_lsf(m17_str_ctx* ctx) {
    DSD_MEMSET(ctx->m17_lsf, 0, sizeof(ctx->m17_lsf));
    DSD_MEMSET(ctx->lsf_chunk, 0, sizeof(ctx->lsf_chunk));

    const uint16_t lsf_fi = m17_compose_frame_info(1, ctx->st, 0, 0, ctx->can, 0, 0);
    for (int i = 0; i < 16; i++) {
        ctx->m17_lsf[96 + i] = (lsf_fi >> (15 - i)) & 1;
    }

    ctx->dst = m17_encode_b40_callsign(ctx->dst, ctx->d40);
    ctx->src = m17_encode_b40_callsign(ctx->src, ctx->s40);

    m17_load_lsf_callsigns(ctx->m17_lsf, ctx->dst, ctx->src);
    ctx->crc_cmp = m17_attach_lsf_crc(ctx->m17_lsf, ctx->lsf_packed);
    m17_encode_lsf_for_rf(ctx->m17_lsf, ctx->m17_lsfs);
}

static int
m17_str_init(m17_str_ctx* ctx, dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(ctx, 0, sizeof(*ctx));
    ctx->opts = opts;
    ctx->state = state;
    ctx->udpport = opts->m17_portno;
    ctx->can = 7;
    ctx->dst = 0;
    ctx->src = 0;
    ctx->st = (state->m17_str_dt == 3) ? 3 : 2;
    DSD_SNPRINTF(ctx->d40, sizeof(ctx->d40), "%s", "BROADCAST");
    DSD_SNPRINTF(ctx->s40, sizeof(ctx->s40), "%s", "DSD-neo  ");
    ctx->nsam = 160;
    ctx->dec = state->m17_rate / 8000;
    ctx->sql_hit = 11;
    ctx->eot_out = 1;
    ctx->new_lsf = 1;

    DSD_MEMSET(mem, 0, M17_RRC_RECOMMENDED_TAPS * sizeof(float));
    DSD_MEMSET(ctx->nil, 0, sizeof(ctx->nil));
    opts->frame_m17 = 1;
    state->m17encoder_tx = 1;
    if (dsd_opts_frontend_active(opts) && state->m17_vox == 0) {
        state->m17encoder_tx = 0;
    }

    if (state->m17_can_en != -1) {
        ctx->can = state->m17_can_en;
    }
    if (state->str50c[0] != 0) {
        DSD_SNPRINTF(ctx->s40, sizeof(ctx->s40), "%s", state->str50c);
    }
    if (state->str50b[0] != 0) {
        DSD_SNPRINTF(ctx->d40, sizeof(ctx->d40), "%s", state->str50b);
    }
    if (strcmp(ctx->d40, "BROADCAST") == 0 || strcmp(ctx->d40, "ALL") == 0) {
        ctx->dst = 0xFFFFFFFFFFFFULL;
    }

    ctx->use_ip = m17_open_udp_if_enabled(opts, state);
    if (ctx->use_ip < 0) {
        return 0;
    }
    m17_send_dead_air_frames(opts, state, ctx->nil, 25);

    dsd_nonce_fill(ctx->sid, sizeof(ctx->sid));

#ifdef USE_CODEC2
    if (ctx->st == 2) {
        ctx->nsam = codec2_samples_per_frame(state->codec2_3200);
    } else if (ctx->st == 3) {
        ctx->nsam = codec2_samples_per_frame(state->codec2_1600);
    }
#endif

    if (!m17_str_prepare_audio_buffers(ctx)) {
        return 0;
    }
    m17_str_prepare_lsf(ctx);

    m17_setup_conn_disc_eotx(ctx->src, 0x41, ctx->conn, ctx->disc, ctx->eotx);
    if (ctx->use_ip == 1) {
        (void)dsd_m17_udp_hook_blaster(opts, state, 11, ctx->conn);
    }
    return 1;
}

static int
m17_str_run_iteration(m17_str_ctx* ctx) {
    m17_str_frame_ctx frame;
    DSD_MEMSET(&frame, 0, sizeof(frame));

    dsd_runtime_pump_controls(ctx->opts, ctx->state);
    m17_str_apply_monitor_side_state(ctx);
    const int read_result = m17_str_read_audio_inputs(ctx);
    if (read_result != M17_STR_READ_OK) {
        return read_result;
    }
    m17_str_update_input_power(ctx);
    m17_str_apply_filters_and_gain(ctx);
    m17_str_encode_codec2(ctx, &frame);
    m17_str_build_stream_frame(ctx, &frame);

    if (ctx->state->m17encoder_tx == 1) {
        m17_str_handle_tx_active(ctx, &frame);
    } else {
        m17_str_handle_tx_idle(ctx, &frame);
    }

    m17_str_iter_tail(ctx);
    return M17_STR_READ_OK;
}

static void
m17_str_finalize(m17_str_ctx* ctx) {
    if (ctx->use_ip == 1) {
        (void)dsd_m17_udp_hook_blaster(ctx->opts, ctx->state, 10, ctx->disc);
    }
    free(ctx->voice1);
    free(ctx->voice2);
    free(ctx->samp1);
    free(ctx->samp2);
}

//encode and create audio of a Project M17 Stream signal
int
encodeM17STR(dsd_opts* opts, dsd_state* state) {
    m17_str_ctx ctx;
    if (!m17_str_init(&ctx, opts, state)) {
        return -1;
    }

    int result = 0;
    while (!exitflag) {
        const int iteration_result = m17_str_run_iteration(&ctx);
        if (iteration_result != M17_STR_READ_OK) {
            result = iteration_result == M17_STR_READ_ERROR ? -1 : 0;
            break;
        }
    }

    m17_str_finalize(&ctx);
    return result;
}

//encode and create audio of a Project M17 BERT signal
void
encodeM17BRT(dsd_opts* opts, dsd_state* state) {

    //initialize RRC memory buffer
    DSD_MEMSET(mem, 0, M17_RRC_RECOMMENDED_TAPS * sizeof(float));

    //NOTE: BERT will not use the nucrses terminal,
    //just strictly for making a BERT test signal

    uint8_t nil[368]; //empty array
    DSD_MEMSET(nil, 0, sizeof(nil));

    int i; //basic utility counter

    //send dead air with type 99
    for (i = 0; i < 25; i++) {
        encodeM17RF(opts, state, nil, 99);
    }

    //send preamble_b for the BERT frame
    encodeM17RF(opts, state, nil, 33);

    uint16_t lfsr = 1; //starting value of the LFSR

    while (!exitflag) {
        // Drain UI commands so queued actions take effect during BERT loop
        dsd_runtime_pump_controls(opts, state);

        // BERT frames contain 197 continuous PRBS9 bits followed by 4 zero flush bits.
        uint8_t m17_b1[M17_BERT_PAYLOAD_BITS + M17_BERT_FLUSH_BITS];
        uint8_t m17_b4s[M17_PAYLOAD_BITS];
        int k = 0;
        int x = 0;
        DSD_MEMSET(m17_b1, 0, sizeof(m17_b1));
        DSD_MEMSET(m17_b4s, 0, sizeof(m17_b4s));
        m17_prbs9_fill_bits(&lfsr, m17_b1, M17_BERT_PAYLOAD_BITS);
        uint16_t consumed = 0U;
        k = (int)m17_bert_encode_type1_bits(m17_b1, m17_b4s, &consumed);
        x = (int)consumed;

        //debug K and X bit positions
        DSD_FPRINTF(stderr, " K: %d; X: %d", k, x);
        m17_print_brt_sequence(m17_b1);

        //convert bit array into symbols and RF/Audio
        encodeM17RF(opts, state, m17_b4s, 3);
    }
}

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
    uint8_t nil[368];
    uint8_t can;
    unsigned long long int dst;
    unsigned long long int src;
    char d40[50];
    char s40[50];
    char text[M17_PACKET_MAX_APPLICATION_BYTES];
    uint8_t m17_lsf[240];
    uint8_t lsf_packed[30];
    uint16_t crc_cmp;
    uint8_t m17_lsfs[368];
    uint8_t m17_p1_full[M17_PACKET_MAX_FRAMES * M17_PACKET_CHUNK_BITS];
    uint8_t m17_p1_packed[M17_PACKET_MAX_TOTAL_BYTES];
    uint16_t app_len;
    int block;
    int ptr;
    int lst;
    uint8_t pbc;
    uint8_t eot;
    int use_ip;
    uint8_t conn[11];
    uint8_t disc[10];
    uint8_t eotx[10];
    uint8_t sid[2];
    uint8_t m17_ip_frame[8000];
    uint8_t m17_ip_packed[34 + M17_PACKET_MAX_APPLICATION_BYTES + M17_PACKET_CRC_BYTES];
} m17_pkt_ctx;

static void
m17_pkt_init_defaults(m17_pkt_ctx* ctx, dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(ctx, 0, sizeof(*ctx));
    ctx->opts = opts;
    ctx->state = state;
    ctx->can = 7;
    DSD_SNPRINTF(ctx->d40, sizeof(ctx->d40), "%s", "BROADCAST");
    DSD_SNPRINTF(ctx->s40, sizeof(ctx->s40), "%s", "DSD-neo  ");
    DSD_SNPRINTF(ctx->text, sizeof(ctx->text), "%s",
                 "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut "
                 "labore et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco "
                 "laboris nisi ut aliquip ex ea commodo consequat. Duis aute irure dolor in reprehenderit in "
                 "voluptate velit esse cillum dolore eu fugiat nulla pariatur. Excepteur sint occaecat cupidatat "
                 "non proident, sunt in culpa qui officia deserunt mollit anim id est laborum.");

    DSD_MEMSET(mem, 0, sizeof(mem));
    DSD_MEMSET(ctx->nil, 0, sizeof(ctx->nil));
    DSD_MEMSET(ctx->m17_lsf, 0, sizeof(ctx->m17_lsf));
    DSD_MEMSET(ctx->m17_lsfs, 0, sizeof(ctx->m17_lsfs));
    DSD_MEMSET(ctx->m17_p1_full, 0, sizeof(ctx->m17_p1_full));
    DSD_MEMSET(ctx->m17_p1_packed, 0, sizeof(ctx->m17_p1_packed));
    DSD_MEMSET(ctx->m17_ip_frame, 0, sizeof(ctx->m17_ip_frame));
    DSD_MEMSET(ctx->m17_ip_packed, 0, sizeof(ctx->m17_ip_packed));
}

static void
m17_pkt_apply_cli_overrides(m17_pkt_ctx* ctx) {
    if (ctx->state->m17_can_en != -1) {
        ctx->can = ctx->state->m17_can_en;
    }
    if (ctx->state->str50c[0] != 0) {
        DSD_SNPRINTF(ctx->s40, sizeof(ctx->s40), "%s", ctx->state->str50c);
    }
    if (ctx->state->str50b[0] != 0) {
        DSD_SNPRINTF(ctx->d40, sizeof(ctx->d40), "%s", ctx->state->str50b);
    }
    if (ctx->state->m17sms[0] != 0) {
        DSD_SNPRINTF(ctx->text, sizeof(ctx->text), "%s", ctx->state->m17sms);
    }
    if (strcmp(ctx->d40, "BROADCAST") == 0 || strcmp(ctx->d40, "ALL") == 0) {
        ctx->dst = 0xFFFFFFFFFFFFULL;
    }
}

static void
m17_pkt_prepare_lsf(m17_pkt_ctx* ctx) {
    const uint16_t lsf_fi = m17_compose_frame_info(0, 0, 0, 0, ctx->can, 0, 0);
    for (int i = 0; i < 16; i++) {
        ctx->m17_lsf[96 + i] = (lsf_fi >> (15 - i)) & 1;
    }

    ctx->dst = m17_encode_b40_callsign(ctx->dst, ctx->d40);
    ctx->src = m17_encode_b40_callsign(ctx->src, ctx->s40);
    m17_load_lsf_callsigns(ctx->m17_lsf, ctx->dst, ctx->src);
    ctx->crc_cmp = m17_attach_lsf_crc(ctx->m17_lsf, ctx->lsf_packed);
    m17_encode_lsf_for_rf(ctx->m17_lsf, ctx->m17_lsfs);
}

static void
m17_pkt_pack_bytes_to_bits(m17_pkt_ctx* ctx) {
    const int padded_bytes = ctx->block * M17_PACKET_CHUNK_BYTES;
    DSD_MEMSET(ctx->m17_p1_full, 0, sizeof(ctx->m17_p1_full));
    for (int byte_index = 0; byte_index < padded_bytes; byte_index++) {
        const uint8_t byte = ctx->m17_p1_packed[byte_index];
        for (int bit = 0; bit < 8; bit++) {
            ctx->m17_p1_full[(byte_index * 8) + bit] = (byte >> (7 - bit)) & 1U;
        }
    }
}

static void
m17_pkt_prepare_payload_layout(m17_pkt_ctx* ctx) {
    const size_t max_sms_text = M17_PACKET_MAX_APPLICATION_BYTES - 2U;
    const size_t text_len = strnlen(ctx->text, max_sms_text);

    DSD_FPRINTF(stderr, "\n SMS: ");
    for (size_t i = 0U; i < text_len; i++) {
        DSD_FPRINTF(stderr, "%c", (uint8_t)ctx->text[i]);
        if ((i % 71U) == 70U) {
            DSD_FPRINTF(stderr, "\n      ");
        }
    }
    DSD_FPRINTF(stderr, "\n");

    DSD_MEMSET(ctx->m17_p1_packed, 0, sizeof(ctx->m17_p1_packed));
    ctx->m17_p1_packed[0] = 0x05U;
    DSD_MEMCPY(&ctx->m17_p1_packed[1], ctx->text, text_len);
    ctx->app_len = (uint16_t)(1U + text_len + 1U);
    ctx->crc_cmp = m17_crc16(ctx->m17_p1_packed, ctx->app_len);
    ctx->m17_p1_packed[ctx->app_len] = (uint8_t)(ctx->crc_cmp >> 8U);
    ctx->m17_p1_packed[ctx->app_len + 1U] = (uint8_t)(ctx->crc_cmp & 0xFFU);

    uint8_t last_frame_bytes = 0U;
    ctx->block = m17_packet_frame_count_for_app_bytes(ctx->app_len, &last_frame_bytes);
    ctx->lst = last_frame_bytes;
    m17_pkt_pack_bytes_to_bits(ctx);
}

static void
m17_pkt_print_full_payload(const m17_pkt_ctx* ctx) {
    DSD_FPRINTF(stderr, "\n M17 Packet      FULL: ");
    for (int i = 0; i < M17_PACKET_CHUNK_BYTES * ctx->block; i++) {
        if ((i % M17_PACKET_CHUNK_BYTES) == 0 && i != 0) {
            DSD_FPRINTF(stderr, "\n                       ");
        }
        DSD_FPRINTF(stderr, "%02X", ctx->m17_p1_packed[i]);
    }
    DSD_FPRINTF(stderr, "\n");
}

static void
m17_pkt_send_mpkt_if_enabled(m17_pkt_ctx* ctx) {
    m17_setup_conn_disc_eotx(ctx->src, 0x41, ctx->conn, ctx->disc, ctx->eotx);
    if (ctx->use_ip == 1) {
        (void)dsd_m17_udp_hook_blaster(ctx->opts, ctx->state, 11, ctx->conn);
    }

    int k = 0;
    for (int j = 0; j < 4; j++) {
        for (int i = 0; i < 8; i++) {
            ctx->m17_ip_frame[k++] = (m17_ip_magic_mpkt[j] >> (7 - i)) & 1;
        }
    }

    dsd_nonce_fill(ctx->sid, sizeof(ctx->sid));
    for (int j = 0; j < 2; j++) {
        for (int i = 0; i < 8; i++) {
            ctx->m17_ip_frame[k++] = (ctx->sid[j] >> (7 - i)) & 1;
        }
    }
    for (int i = 0; i < 224; i++) {
        ctx->m17_ip_frame[k++] = ctx->m17_lsf[i];
    }

    for (int i = 0; i < 34; i++) {
        ctx->m17_ip_packed[i] = (uint8_t)convert_bits_into_output(&ctx->m17_ip_frame[((size_t)i * 8)], 8);
    }
    for (uint16_t i = 0U; i < ctx->app_len; i++) {
        ctx->m17_ip_packed[i + 34] = ctx->m17_p1_packed[i];
    }

    const uint16_t ip_crc = m17_crc16(ctx->m17_ip_packed, (uint16_t)(34U + ctx->app_len));
    uint8_t crc_bits[16];
    DSD_MEMSET(crc_bits, 0, sizeof(crc_bits));
    for (int i = 0; i < 16; i++) {
        crc_bits[i] = (ip_crc >> (15 - i)) & 1;
    }
    for (int i = (int)(ctx->app_len + 34U), j = 0; i < (int)(ctx->app_len + 34U + 2U); i++, j++) {
        ctx->m17_ip_packed[i] = (uint8_t)convert_bits_into_output(&crc_bits[(size_t)j * 8], 8);
    }

    if (ctx->use_ip == 1) {
        const int udp_len = (int)(ctx->app_len + 34U + 2U);
        const int udp_return = dsd_m17_udp_hook_blaster(ctx->opts, ctx->state, udp_len, ctx->m17_ip_packed);
        DSD_FPRINTF(stderr, " UDP IP Frame CRC: %04X; UDP RETURN: %d: LEN: %d; SENT: %d;", ip_crc, udp_return,
                    ctx->app_len, udp_len);
        (void)dsd_m17_udp_hook_blaster(ctx->opts, ctx->state, 10, ctx->eotx);
        (void)dsd_m17_udp_hook_blaster(ctx->opts, ctx->state, 10, ctx->disc);
    }
}

static void
m17_pkt_send_lsf_once(m17_pkt_ctx* ctx, int* new_lsf) {
    if (*new_lsf != 1) {
        return;
    }
    DSD_FPRINTF(stderr, "\n M17 LSF    (ENCODER): ");
    m17_monitor_encoded_lsf(ctx->opts, ctx->state, ctx->m17_lsfs);
    DSD_MEMSET(ctx->nil, 0, sizeof(ctx->nil));
    encodeM17RF(ctx->opts, ctx->state, ctx->nil, 11);
    encodeM17RF(ctx->opts, ctx->state, ctx->m17_lsfs, 1);
    *new_lsf = 0;
    ctx->ptr = 0;
}

static void
m17_pkt_send_data_frame(m17_pkt_ctx* ctx) {
    uint8_t m17_p1[M17_PACKET_TYPE1_FLUSH_BITS];
    uint8_t m17_p4s[M17_PAYLOAD_BITS];
    DSD_MEMSET(m17_p1, 0, sizeof(m17_p1));
    DSD_MEMSET(m17_p4s, 0, sizeof(m17_p4s));

    const int chunk_bit_offset = ctx->ptr;
    ctx->ptr += M17_PACKET_CHUNK_BITS;
    if (ctx->ptr / 8 >= ctx->block * M17_PACKET_CHUNK_BYTES) {
        ctx->eot = 1;
        ctx->pbc = ctx->lst;
    }
    uint8_t metadata_byte = 0U;
    (void)m17_packet_metadata_byte(ctx->eot, ctx->pbc, &metadata_byte);
    m17_packet_build_type1_bits(&ctx->m17_p1_full[chunk_bit_offset], metadata_byte, m17_p1);
    (void)m17_packet_encode_type1_bits(m17_p1, m17_p4s, NULL);

    DSD_FPRINTF(stderr, "\n M17 Packet (ENCODER): ");
    for (int i = 0; i < 26; i++) {
        DSD_FPRINTF(stderr, "%02X", (uint8_t)convert_bits_into_output(&m17_p1[((size_t)i * 8)], 8));
    }
    encodeM17RF(ctx->opts, ctx->state, m17_p4s, 4);

    if (ctx->eot) {
        DSD_MEMSET(ctx->nil, 0, sizeof(ctx->nil));
        encodeM17RF(ctx->opts, ctx->state, ctx->nil, 55);
        m17_send_dead_air_frames(ctx->opts, ctx->state, ctx->nil, 25);
        exitflag = 1;
    }
    ctx->pbc++;
}

static void
m17_pkt_run_loop(m17_pkt_ctx* ctx) {
    int new_lsf = 1;
    while (!exitflag) {
        dsd_runtime_pump_controls(ctx->opts, ctx->state);
        m17_pkt_send_lsf_once(ctx, &new_lsf);
        m17_pkt_send_data_frame(ctx);
    }
}

//encode and create audio of a Project M17 PKT signal
int
encodeM17PKT(dsd_opts* opts, dsd_state* state) {
    m17_pkt_ctx ctx;
    m17_pkt_init_defaults(&ctx, opts, state);
    m17_pkt_apply_cli_overrides(&ctx);
    ctx.use_ip = m17_open_udp_if_enabled(opts, state);
    if (ctx.use_ip < 0) {
        return -1;
    }
    m17_send_dead_air_frames(opts, state, ctx.nil, 25);
    m17_pkt_prepare_lsf(&ctx);
    m17_pkt_prepare_payload_layout(&ctx);
    m17_pkt_print_full_payload(&ctx);
    m17_pkt_send_mpkt_if_enabled(&ctx);
    m17_pkt_run_loop(&ctx);
    return 0;
}

static void
m17_decode_pkt_print_text(const char* label, const uint8_t* input, int len) {
    DSD_FPRINTF(stderr, "%s", label);
    for (int i = 0; i < len; i++) {
        if (input[i] == '\0') {
            break;
        }
        DSD_FPRINTF(stderr, "%c", input[i]);
        if (((i + 1) % 71) == 0) {
            DSD_FPRINTF(stderr, "\n      ");
        }
    }
}

static void
m17_decode_pkt_print_extended_csd(const uint8_t* input, int len) {
    struct m17_extended_callsign_result ext;
    const size_t input_len = (len > 0) ? (size_t)len : 0U;
    if (m17_parse_extended_callsign_meta(input, input_len, &ext) != 0) {
        DSD_FPRINTF(stderr, " Invalid Extended Callsign META;");
        return;
    }

    DSD_FPRINTF(stderr, " CF1: %s", ext.field1_csd);
    if (ext.has_field2 != 0U) {
        DSD_FPRINTF(stderr, " REF: %s", ext.field2_csd);
    }
}

static void
m17_decode_pkt_print_gnss_source(uint8_t data_source) {
    switch (data_source) {
        case 0: DSD_FPRINTF(stderr, " M17 Client;"); break;
        case 1: DSD_FPRINTF(stderr, " OpenRTX;"); break;
        default: DSD_FPRINTF(stderr, " Other Data Source: %0X;", data_source); break;
    }
}

static void
m17_decode_pkt_print_station_type(uint8_t station_type) {
    switch (station_type) {
        case 0: DSD_FPRINTF(stderr, " Fixed Station;"); break;
        case 1: DSD_FPRINTF(stderr, " Mobile Station;"); break;
        case 2: DSD_FPRINTF(stderr, " Handheld;"); break;
        default: DSD_FPRINTF(stderr, " Reserved Station Type: %02X;", station_type); break;
    }
}

static void
m17_decode_pkt_print_gnss(const uint8_t* input, int len) {
    struct m17_gnss_result gnss;
    const size_t input_len = (len > 0) ? (size_t)len : 0U;
    if (m17_parse_gnss_v2(input, input_len, &gnss) != 0) {
        DSD_FPRINTF(stderr, " Invalid GNSS packet;");
        return;
    }

    if ((gnss.validity & 0x8U) != 0U) {
        DSD_FPRINTF(stderr, "\n GPS: (%f, %f);", gnss.latitude_deg, gnss.longitude_deg);
    } else {
        DSD_FPRINTF(stderr, "\n GPS Not Valid;");
    }

    if ((gnss.validity & 0x4U) != 0U) {
        DSD_FPRINTF(stderr, " Altitude: %.1f m;", gnss.altitude_m);
    }
    if ((gnss.validity & 0x2U) != 0U) {
        DSD_FPRINTF(stderr, " Speed: %.1f km/h;", gnss.speed_kmh);
        DSD_FPRINTF(stderr, " Bearing: %u Degrees;", gnss.bearing_deg);
    }
    if ((gnss.validity & 0x1U) != 0U) {
        DSD_FPRINTF(stderr, "\n      Radius: %.1f;", gnss.radius_m);
    }
    if (gnss.invalid_zero_fields != 0U) {
        DSD_FPRINTF(stderr, " Nonzero Invalid Fields: %X;", gnss.invalid_zero_fields);
    }

    m17_decode_pkt_print_gnss_source(gnss.data_source);
    m17_decode_pkt_print_station_type(gnss.station_type);
}

static void
m17_decode_pkt_print_meta_or_arb(dsd_state* state, const uint8_t* input, int len, uint32_t protocol) {
    DSD_FPRINTF(stderr, " ");
    if (protocol == 0x80U) {
        if (len < (int)M17_META_BYTES) {
            DSD_FPRINTF(stderr, " Invalid Text META;");
            return;
        }
        struct m17_meta_text_block block;
        if (m17_meta_text_parse_block(input, &block) != 0) {
            DSD_FPRINTF(stderr, " Invalid Text META;");
            return;
        }
        if (block.has_text == 0U) {
            DSD_FPRINTF(stderr, " No Text Data;");
            return;
        }

        DSD_FPRINTF(stderr, "%u/%u; ", (unsigned)(block.block_index + 1U), (unsigned)block.total_blocks);
        for (uint8_t i = 0U; i < M17_TEXT_BLOCK_BYTES; i++) {
            DSD_FPRINTF(stderr, "%c", block.text[i]);
        }

        if (state != NULL) {
            struct m17_meta_text_assembler assembler;
            assembler.control_or = state->m17_text_meta_control_or;
            assembler.expected_bitmap = state->m17_text_meta_expected_bitmap;
            assembler.received_bitmap = state->m17_text_meta_received_bitmap;
            DSD_MEMCPY(assembler.text, state->m17_text_meta, M17_TEXT_MAX_BYTES);

            char text[M17_TEXT_MAX_BYTES + 1U];
            uint8_t text_len = 0U;
            DSD_MEMSET(text, 0, sizeof(text));
            const int complete = m17_meta_text_assembler_push(&assembler, &block, text, &text_len);
            state->m17_text_meta_control_or = assembler.control_or;
            state->m17_text_meta_expected_bitmap = assembler.expected_bitmap;
            state->m17_text_meta_received_bitmap = assembler.received_bitmap;
            DSD_MEMCPY(state->m17_text_meta, assembler.text, M17_TEXT_MAX_BYTES);
            if (complete > 0) {
                DSD_FPRINTF(stderr, " Complete: %s", text);
            }
        }
        return;
    }

    if (protocol == 0x83U && len > 0) {
        uint8_t segment_num = 1U;
        uint8_t segment_len = 1U;
        (void)m17_meta_text_segment_info((uint8_t)protocol, input[0], &segment_num, &segment_len);
        DSD_FPRINTF(stderr, "%d/%d; ", segment_num, segment_len);
        for (int i = 1; i < len; i++) {
            DSD_FPRINTF(stderr, "%c", input[i]);
        }
        return;
    }

    for (int i = 0; i < len; i++) {
        DSD_FPRINTF(stderr, "%c", input[i]);
    }
}

static void
m17_decode_pkt_print_hex(const uint8_t* input, int len) {
    DSD_FPRINTF(stderr, " ");
    for (int i = 0; i < len; i++) {
        DSD_FPRINTF(stderr, "%02X", input[i]);
    }
}

int
m17_decode_pkt_should_report_encrypted(const dsd_state* state, uint32_t protocol) {
    if (state == NULL || state->m17_enc == 0 || state->m17_payload_decrypted != 0U) {
        return 0;
    }
    if (protocol == 0x69U || (protocol >= 0x80U && protocol <= 0x83U)) {
        return 0;
    }
    return 1;
}

static void
m17_decode_pkt_dispatch_payload(dsd_state* state, const uint8_t* input, int len, const uint8_t* payload,
                                int payload_len, uint32_t protocol) {
    switch (protocol) {
        case 0x05: m17_decode_pkt_print_text("\n SMS: ", payload, payload_len); break;
        case 0x07: m17_decode_pkt_print_text(" TLE:\n", payload, payload_len); break;
        case 0x82: m17_decode_pkt_print_extended_csd(input, len); break;
        case 0x81:
        case 0x91: m17_decode_pkt_print_gnss(input, len); break;
        case 0x80:
        case 0x83:
        case 0x89:
        case 0x99: m17_decode_pkt_print_meta_or_arb(state, payload, payload_len, protocol); break;
        default: m17_decode_pkt_print_hex(payload, payload_len); break;
    }
}

static void
decodeM17PKT(const dsd_opts* opts, dsd_state* state, const uint8_t* input, int len) {
    //Decode the completed packet
    UNUSED(opts);
    if (input == NULL || len <= 0) {
        return;
    }

    struct m17_packet_protocol_result protocol_spec;
    if (m17_packet_protocol_decode(input, (size_t)len, &protocol_spec) != 0) {
        DSD_FPRINTF(stderr, " Protocol: Invalid;");
        m17_decode_pkt_print_hex(input, len);
        return;
    }

    const uint32_t protocol = protocol_spec.identifier;
    const uint8_t* payload = input + protocol_spec.length;
    const int payload_len = len - (int)protocol_spec.length;
    const char* protocol_name = m17_packet_protocol_name_u32(protocol);
    DSD_FPRINTF(stderr, " Protocol:");
    if (protocol_name != NULL) {
        DSD_FPRINTF(stderr, " %s;", protocol_name);
    } else {
        DSD_FPRINTF(stderr, " Res/Unk: %06X;", protocol);
    }

    if (!m17_can_matches_state(state)) {
        DSD_FPRINTF(stderr, " CAN Filtered;");
        return;
    }

    if (m17_decode_pkt_should_report_encrypted(state, protocol)) {
        DSD_FPRINTF(stderr, " *Encrypted*");
        return;
    }

    m17_decode_pkt_dispatch_payload(state, input, len, payload, payload_len, protocol);
}

static void
m17_soft_depuncture_p3(const uint16_t* soft_bits, uint16_t* depunc) {
    int bit_index = 0;
    for (int i = 0; i < M17_PACKET_TYPE2_BITS; i++) {
        if (m17_puncture_pattern_3[i % M17_PUNCTURE_P3_LEN] == 1) {
            depunc[i] = soft_bits[bit_index++];
        } else {
            depunc[i] = 0x7FFF;
        }
    }
}

int
m17_pkt_ptr_clamped(int pbc_count) {
    int ptr = pbc_count * M17_PACKET_CHUNK_BYTES;
    if (ptr > M17_PACKET_MAX_TOTAL_BYTES) {
        return M17_PACKET_MAX_TOTAL_BYTES;
    }
    if (ptr < 0) {
        return 0;
    }
    return ptr;
}

static void
m17_pkt_log_counter(int pbc_count, uint8_t counter, uint8_t eot) {
    if (eot == 0U) {
        DSD_FPRINTF(stderr, " CNT: %02d; PBC: %02d; EOT: %d;", pbc_count, counter, eot);
    } else {
        DSD_FPRINTF(stderr, " CNT: %02d; LST: %02d; EOT: %d;", pbc_count, counter, eot);
    }
}

static void
m17_pkt_log_frame_if_enabled(const dsd_opts* opts, const uint8_t* pkt_packed) {
    if (opts->payload != 1) {
        return;
    }
    DSD_FPRINTF(stderr, "\n pkt: ");
    for (int i = 0; i < 26; i++) {
        DSD_FPRINTF(stderr, " %02X", pkt_packed[i]);
    }
}

static void
m17_pkt_log_final_if_enabled(const dsd_opts* opts, const dsd_state* state, int end, uint16_t crc_cmp,
                             uint16_t crc_ext) {
    if (opts->payload != 1) {
        return;
    }

    DSD_FPRINTF(stderr, "\n PKT:");
    for (int i = 0; i < end; i++) {
        if ((i % M17_PACKET_CHUNK_BYTES) == 0 && i != 0) {
            DSD_FPRINTF(stderr, "\n     ");
        }
        DSD_FPRINTF(stderr, " %02X", state->m17_pkt[i]);
    }
    DSD_FPRINTF(stderr, "\n      CRC - C: %04X; E: %04X", crc_cmp, crc_ext);
}

void
m17_pkt_finalize_eot(const dsd_opts* opts, dsd_state* state, uint16_t app_len, int end) {
    if (opts == NULL || state == NULL) {
        return;
    }
    if (app_len > M17_PACKET_MAX_APPLICATION_BYTES) {
        app_len = M17_PACKET_MAX_APPLICATION_BYTES;
    }
    const uint16_t crc_cmp = m17_crc16(state->m17_pkt, app_len);
    const uint16_t crc_ext = (uint16_t)(((uint16_t)state->m17_pkt[app_len] << 8U) | state->m17_pkt[app_len + 1U]);

    if ((crc_cmp == crc_ext || opts->aggressive_framesync == 0) && app_len > 0U) {
        decodeM17PKT(opts, state, state->m17_pkt, app_len);
    }
    if (crc_cmp != crc_ext) {
        DSD_FPRINTF(stderr, " (CRC ERR) ");
    }

    m17_pkt_log_final_if_enabled(opts, state, end, crc_cmp, crc_ext);
    DSD_MEMSET(state->m17_pkt, 0, sizeof(state->m17_pkt));
    state->m17_pbc_ct = 0;
}

//WIP PKT decoder - soft symbol enhanced
void
processM17PKT(dsd_opts* opts, dsd_state* state) {

    float soft_symbols[M17_PAYLOAD_SYMBOLS];    //Raw float symbol values for soft-decision Viterbi
    uint16_t m17_soft_bits[M17_PAYLOAD_BITS];   //368 soft costs (de-interleaved and de-scrambled)
    uint16_t m17_depunc[M17_PACKET_TYPE2_BITS]; //420 weighted values after depuncturing
    uint8_t pkt_packed[50];
    uint8_t pkt_bytes[48];

    DSD_MEMSET(soft_symbols, 0, sizeof(soft_symbols));
    DSD_MEMSET(state->m17_lsf, 0, sizeof(state->m17_lsf));
    DSD_MEMSET(m17_soft_bits, 0, sizeof(m17_soft_bits));
    DSD_MEMSET(m17_depunc, 0, sizeof(m17_depunc));

    DSD_MEMSET(pkt_packed, 0, sizeof(pkt_packed));
    DSD_MEMSET(pkt_bytes, 0, sizeof(pkt_bytes));

    m17_capture_soft_symbols(opts, state, soft_symbols);
    m17_soft_bits_from_symbols(soft_symbols, state, m17_soft_bits);
    m17_soft_depuncture_p3(m17_soft_bits, m17_depunc);
    (void)viterbi_decode(pkt_bytes, m17_depunc, M17_PACKET_TYPE2_BITS);
    DSD_MEMCPY(pkt_packed, pkt_bytes + 1, 26);

    uint8_t counter = 0U;
    uint8_t eot = 0U;
    const int metadata_ok = m17_packet_parse_metadata_byte(pkt_packed[25], &eot, &counter);
    if (metadata_ok != 0) {
        DSD_FPRINTF(stderr, " PKT metadata invalid;");
        DSD_MEMSET(state->m17_pkt, 0, sizeof(state->m17_pkt));
        state->m17_pbc_ct = 0;
        DSD_FPRINTF(stderr, "\n");
        return;
    }

    const int ptr = m17_pkt_ptr_clamped(state->m17_pbc_ct);
    if (eot == 0U && counter != (uint8_t)state->m17_pbc_ct) {
        DSD_FPRINTF(stderr, " PKT counter mismatch: expected %02d got %02u;", state->m17_pbc_ct, counter);
        DSD_MEMSET(state->m17_pkt, 0, sizeof(state->m17_pkt));
        state->m17_pbc_ct = 0;
        DSD_FPRINTF(stderr, "\n");
        return;
    }

    uint16_t app_len = 0U;
    int total_with_crc = ptr + M17_PACKET_CHUNK_BYTES;
    if (eot != 0U) {
        if (m17_packet_app_bytes_from_eof((uint8_t)state->m17_pbc_ct, counter, &app_len) != 0) {
            DSD_FPRINTF(stderr, " PKT EOF byte count invalid;");
            DSD_MEMSET(state->m17_pkt, 0, sizeof(state->m17_pkt));
            state->m17_pbc_ct = 0;
            DSD_FPRINTF(stderr, "\n");
            return;
        }
        total_with_crc = ptr + counter;
    }

    m17_pkt_log_counter(state->m17_pbc_ct, counter, eot);
    DSD_MEMCPY(state->m17_pkt + ptr, pkt_packed, M17_PACKET_CHUNK_BYTES);
    m17_pkt_log_frame_if_enabled(opts, pkt_packed);

    if (eot != 0U) {
        m17_pkt_finalize_eot(opts, state, app_len, total_with_crc);
    }

    if (eot == 0U) {
        if (state->m17_pbc_ct >= (M17_PACKET_MAX_FRAMES - 1)) {
            DSD_FPRINTF(stderr, " PKT frame count overflow;");
            DSD_MEMSET(state->m17_pkt, 0, sizeof(state->m17_pkt));
            state->m17_pbc_ct = 0;
            DSD_FPRINTF(stderr, "\n");
            return;
        }
        state->m17_pbc_ct++;
    }

    //ending linebreak
    DSD_FPRINTF(stderr, "\n");

} //end processM17PKT

static const uint8_t m17_ip_ackn[4] = {0x41, 0x43, 0x4B, 0x4E};
static const uint8_t m17_ip_nack[4] = {0x4E, 0x41, 0x43, 0x4B};
static const uint8_t m17_ip_conn[4] = {0x43, 0x4F, 0x4E, 0x4E};
static const uint8_t m17_ip_disc[4] = {0x44, 0x49, 0x53, 0x43};
static const uint8_t m17_ip_ping[4] = {0x50, 0x49, 0x4E, 0x47};
static const uint8_t m17_ip_pong[4] = {0x50, 0x4F, 0x4E, 0x47};
static const uint8_t m17_ip_eotx[4] = {0x45, 0x4F, 0x54, 0x58};

typedef struct m17_ip_ctrl_desc {
    const uint8_t* magic;
    const char* label;
    int dump_len;
    int print_source;
    int print_module;
    int drop_carrier;
} m17_ip_ctrl_desc;

static void
m17_ip_bits_from_54_bytes(const uint8_t* ip_frame, uint8_t* ip_bits) {
    int k = 0;
    for (int i = 0; i < 54; i++) {
        for (int j = 0; j < 8; j++) {
            ip_bits[k++] = (ip_frame[i] >> (7 - j)) & 1U;
        }
    }
}

static void
m17_ip_dump_bytes_wrapped(const uint8_t* ip_frame, int len, int wrap, const char* indent) {
    for (int i = 0; i < len; i++) {
        if ((i % wrap) == 0) {
            DSD_FPRINTF(stderr, "\n%s", indent);
        }
        DSD_FPRINTF(stderr, "[%02X]", ip_frame[i]);
    }
}

static void
m17_ip_dump_bytes_spaced(const uint8_t* ip_frame, int len, int wrap, const char* indent) {
    for (int i = 0; i < len; i++) {
        if ((i % wrap) == 0) {
            DSD_FPRINTF(stderr, "\n%s", indent);
        }
        DSD_FPRINTF(stderr, "%02X ", ip_frame[i]);
    }
}

static void
m17_ip_copy_lsf_from_bits(dsd_state* state, const uint8_t* ip_bits) {
    for (int i = 0; i < 224; i++) {
        state->m17_lsf[i] = ip_bits[i + 48];
    }
}

static void
m17_ip_handle_stream_frame(const dsd_opts* opts, dsd_state* state, const uint8_t* ip_frame) {
    uint8_t ip_bits[462];
    uint8_t payload[128];
    DSD_MEMSET(ip_bits, 0, sizeof(ip_bits));
    DSD_MEMSET(payload, 0, sizeof(payload));
    m17_ip_bits_from_54_bytes(ip_frame, ip_bits);

    state->carrier = 1;
    state->synctype = DSD_SYNC_M17_STR_POS;

    const uint16_t sid = (uint16_t)convert_bits_into_output(&ip_bits[32], 16);
    m17_ip_copy_lsf_from_bits(state, ip_bits);

    const uint16_t fn = (uint16_t)convert_bits_into_output(&ip_bits[273], 15);
    const uint8_t eot = ip_bits[272];
    const uint16_t stream_frame_number = (uint16_t)((eot != 0U ? M17_STREAM_FRAME_END_MASK : 0U) | fn);
    m17_store_current_aes_counter(state, stream_frame_number);
    DSD_FPRINTF(stderr, "\n M17 IP Stream: %04X; FN: %05d;", sid, fn);
    if (eot) {
        DSD_FPRINTF(stderr, " EOT;");
    }

    for (int i = 0; i < 128; i++) {
        payload[i] = ip_bits[i + 288];
    }

    const uint16_t crc_ext = (uint16_t)((ip_frame[52] << 8) + ip_frame[53]);
    const uint16_t crc_cmp = m17_crc16(ip_frame, 52);
    if (crc_ext == crc_cmp) {
        M17decodeLSF(state);
    }
    uint8_t processed_payload[M17_STREAM_PAYLOAD_BITS];
    (void)m17_dispatch_stream_payload(opts, state, payload, stream_frame_number, processed_payload);

    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n IP:");
        m17_ip_dump_bytes_wrapped(ip_frame, 54, 14, "    ");
        DSD_FPRINTF(stderr, " (CRC CHK) E: %04X; C: %04X;", crc_ext, crc_cmp);
    }
    if (crc_ext != crc_cmp) {
        DSD_FPRINTF(stderr, " IP CRC ERR");
    }
}

static int
m17_ip_handle_control_frame(const m17_ip_ctrl_desc* desc, const dsd_opts* opts, dsd_state* state,
                            const uint8_t* ip_frame) {
    if (memcmp(ip_frame, desc->magic, 4) != 0) {
        return 0;
    }

    DSD_FPRINTF(stderr, "\n M17 IP   %s: ", desc->label);
    if (desc->print_source) {
        M17printIpSource(m17_read_ip_source(ip_frame));
    }
    if (desc->print_module) {
        DSD_FPRINTF(stderr, "Module: %c; ", ip_frame[10]);
    }
    if (opts->payload == 1) {
        m17_ip_dump_bytes_spaced(ip_frame, desc->dump_len, desc->dump_len, "");
    }
    if (desc->drop_carrier) {
        state->carrier = 0;
        state->synctype = DSD_SYNC_NONE;
    }
    return 1;
}

static void
m17_ip_handle_mpkt_frame(const dsd_opts* opts, dsd_state* state, const uint8_t* ip_frame, int err) {
    if (err < 37) {
        return;
    }

    uint8_t ip_bits[462];
    DSD_MEMSET(ip_bits, 0, sizeof(ip_bits));
    m17_ip_bits_from_54_bytes(ip_frame, ip_bits);
    const uint16_t sid = (uint16_t)convert_bits_into_output(&ip_bits[32], 16);
    m17_ip_copy_lsf_from_bits(state, ip_bits);

    const uint16_t crc_ext = (uint16_t)((ip_frame[err - 2] << 8) + ip_frame[err - 1]);
    const uint16_t crc_cmp = m17_crc16(ip_frame, (uint16_t)(err - 2));
    DSD_FPRINTF(stderr, "\n M17 IP   MPKT: %04X;", sid);

    if (crc_ext == crc_cmp) {
        M17decodeLSF(state);
    }
    if (opts->payload == 1) {
        m17_ip_dump_bytes_spaced(ip_frame, err, 25, "                ");
        DSD_FPRINTF(stderr, " (CRC CHK) E: %04X; C: %04X;", crc_ext, crc_cmp);
        DSD_FPRINTF(stderr, "\n M17 IP   RECD: %d", err);
    }
    if (crc_ext == crc_cmp) {
        decodeM17PKT(opts, state, ip_frame + 34, err - 34 - 3);
    } else {
        DSD_FPRINTF(stderr, " IP CRC ERR");
    }
}

void
m17_ip_dispatch_frame(const dsd_opts* opts, dsd_state* state, const uint8_t* ip_frame, int len) {
    if (opts == NULL || state == NULL || ip_frame == NULL) {
        return;
    }
    if (memcmp(ip_frame, m17_ip_magic_stream, 4) == 0) {
        m17_ip_handle_stream_frame(opts, state, ip_frame);
        return;
    }

    if (memcmp(ip_frame, m17_ip_magic_mpkt, 4) == 0) {
        m17_ip_handle_mpkt_frame(opts, state, ip_frame, len);
        return;
    }

    static const m17_ip_ctrl_desc ctrl[] = {
        {m17_ip_ackn, "ACKN", 0, 0, 0, 0},  {m17_ip_nack, "NACK", 0, 0, 0, 0},  {m17_ip_conn, "CONN", 11, 1, 1, 0},
        {m17_ip_disc, "DISC", 10, 1, 0, 1}, {m17_ip_eotx, "EOTX", 10, 1, 0, 1}, {m17_ip_ping, "PING", 10, 1, 0, 0},
        {m17_ip_pong, "PONG", 10, 1, 0, 0},
    };

    for (size_t i = 0; i < sizeof(ctrl) / sizeof(ctrl[0]); i++) {
        if (m17_ip_handle_control_frame(&ctrl[i], opts, state, ip_frame)) {
            return;
        }
    }
}

//Process Received IP Frames
int
processM17IPF(dsd_opts* opts, dsd_state* state) {
    opts->dmr_stereo = 0;
    opts->audio_in_type = AUDIO_IN_NULL;
    opts->udp_sockfd = dsd_m17_udp_hook_udp_bind(opts->m17_hostname, opts->m17_portno);

    uint8_t ip_frame[1000];
    DSD_MEMSET(ip_frame, 0, sizeof(ip_frame));

    if (!dsd_m17_udp_socket_is_valid(opts->udp_sockfd)) {
        return -1;
    }

    while (!exitflag) {
        dsd_runtime_pump_controls(opts, state);

        const int err = dsd_m17_udp_hook_receiver(opts, &ip_frame);
        if (err < 0) {
            return -1;
        }
        if (err > 0) {
            m17_ip_dispatch_frame(opts, state, ip_frame, err);
        }

        if (dsd_opts_frontend_active(opts)) {
            dsd_telemetry_publish_both_and_redraw(opts, state);
        }
        watchdog_event_history(opts, state, 0);
        watchdog_event_current(opts, state, 0);
        DSD_MEMSET(ip_frame, 0, sizeof(ip_frame));
    }
    return 0;
}
