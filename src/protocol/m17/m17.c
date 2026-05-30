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
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/audio_filters.h>
#include <dsd-neo/core/cleanup.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/fec/viterbi.h>
#include <dsd-neo/platform/audio.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/nonce.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
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
#include <dsd-neo/runtime/telemetry.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <math.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#ifdef USE_RADIO
#endif

#ifdef USE_CODEC2
#include <codec2/codec2.h>
#endif

static void decodeM17PKT(const dsd_opts* opts, const dsd_state* state, const uint8_t* input, int len);
static void M17decodeLSF(dsd_state* state);
static void M17decodeLSFFields(dsd_state* state, const struct m17_lsf_result* res);
static void M17logLSFSummary(dsd_state* state, const struct m17_lsf_result* res);
static void M17storeLSFMeta(dsd_state* state, const struct m17_lsf_result* res);
static void M17decodeMetaPayload(dsd_state* state, uint8_t identifier);
static void M17decodeLSFMeta(dsd_state* state, const struct m17_lsf_result* res);
static void M17logLSFTrailer(const dsd_state* state, const struct m17_lsf_result* res);

#ifdef USE_CODEC2
#define M17_CODEC2_OPTS_PARAM  dsd_opts*
#define M17_CODEC2_STATE_PARAM dsd_state*
#else
#define M17_CODEC2_OPTS_PARAM  const dsd_opts*
#define M17_CODEC2_STATE_PARAM const dsd_state*
#endif

static inline short
clip_float_to_short(float v) {
    if (v > 32767.0f) {
        return 32767;
    }
    if (v < -32768.0f) {
        return -32768;
    }
    return (short)lrintf(v);
}

static void
m17_assign_stream_id(uint8_t sid[2]) {
    dsd_nonce_fill(sid, 2);
}

//from M17_Implementations / libM17 -- sp5wwp -- should have just looked here to begin with
//this setup looks very similar to the OP25 variant of crc16, but with a few differences (uses packed bytes)
static uint16_t
crc16m17(const uint8_t* in, const uint16_t len) {
    uint32_t crc = 0xFFFF; //init val
    uint16_t poly = 0x5935;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= in[i] << 8;
        for (uint8_t j = 0; j < 8; j++) {
            crc <<= 1;
            if (crc & 0x10000) {
                crc = (crc ^ poly) & 0xFFFF;
            }
        }
    }

    return crc & (0xFFFF);
}

static void
M17decodeCSD(dsd_state* state, unsigned long long int dst, unsigned long long int src) {
    //evaluate dst and src, and determine if they need to be converted to callsign
    int i;
    char c;
    DSD_MEMSET(state->m17_dst_csd, 0, sizeof(state->m17_dst_csd));
    DSD_MEMSET(state->m17_src_csd, 0, sizeof(state->m17_src_csd));
    if (dst == 0xFFFFFFFFFFFF) {
        DSD_FPRINTF(stderr, " DST: BROADCAST");
    } else if (dst == 0 || dst >= 0xEE6B28000000) {
        DSD_FPRINTF(stderr, " DST: RESERVED %012llx", dst);
    } else {
        DSD_FPRINTF(stderr, " DST: ");
        for (i = 0; i < 9; i++) {
            if (dst == 0) {
                break;
            }
            c = m17_base40_alphabet[dst % 40];
            state->m17_dst_csd[i] = c;
            DSD_FPRINTF(stderr, "%c", c);
            dst = dst / 40;
        }
        //assign completed CSD to a more useful string instead
        DSD_SNPRINTF(state->m17_dst_str, sizeof(state->m17_dst_str), "%c%c%c%c%c%c%c%c%c", state->m17_dst_csd[0],
                     state->m17_dst_csd[1], state->m17_dst_csd[2], state->m17_dst_csd[3], state->m17_dst_csd[4],
                     state->m17_dst_csd[5], state->m17_dst_csd[6], state->m17_dst_csd[7], state->m17_dst_csd[8]);

        //debug
    }

    if (src == 0xFFFFFFFFFFFF) {
        DSD_FPRINTF(stderr, " SRC:  UNKNOWN FFFFFFFFFFFF");
    } else if (src == 0 || src >= 0xEE6B28000000) {
        DSD_FPRINTF(stderr, " SRC: RESERVED %012llx", src);
    } else {
        DSD_FPRINTF(stderr, " SRC: ");
        for (i = 0; i < 9; i++) {
            if (src == 0) {
                break;
            }
            c = m17_base40_alphabet[src % 40];
            state->m17_src_csd[i] = c;
            DSD_FPRINTF(stderr, "%c", c);
            src = src / 40;
        }
        //assign completed CSD to a more useful string instead
        DSD_SNPRINTF(state->m17_src_str, sizeof(state->m17_src_str), "%c%c%c%c%c%c%c%c%c", state->m17_src_csd[0],
                     state->m17_src_csd[1], state->m17_src_csd[2], state->m17_src_csd[3], state->m17_src_csd[4],
                     state->m17_src_csd[5], state->m17_src_csd[6], state->m17_src_csd[7], state->m17_src_csd[8]);

        //debug
    }

    //debug
}

static uint16_t
M17composeFrameInfo(uint16_t ps, uint16_t dt, uint16_t et, uint16_t es, uint16_t cn, uint16_t rs) {
    return (uint16_t)((ps & 0x1U) | ((dt & 0x3U) << 1) | ((et & 0x3U) << 3) | ((es & 0x3U) << 5) | ((cn & 0xFU) << 7)
                      | ((rs & 0x1FU) << 11));
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

static void
M17logV3PayloadContents(uint8_t payload_contents) {
    switch (payload_contents) {
        case 0x1U: LOG_INFO(" Stream Data"); break;
        case 0x2U: LOG_INFO(" Voice (3200bps)"); break;
        case 0x3U: LOG_INFO(" Voice (1600bps)"); break;
        case 0xFU: LOG_INFO(" Packet Data"); break;
        default: LOG_INFO(" Reserved: %X", payload_contents); break;
    }
}

static void
M17logV3Encryption(uint8_t encryption_type) {
    if (encryption_type == 0U) {
        return;
    }

    LOG_INFO(" ENC:");
    switch (encryption_type) {
        case 0x1U: LOG_INFO(" Scrambler (8-bit);"); break;
        case 0x2U: LOG_INFO(" Scrambler (16-bit);"); break;
        case 0x3U: LOG_INFO(" Scrambler (24-bit);"); break;
        case 0x4U: LOG_INFO(" AES-CTR (128-bit);"); break;
        case 0x5U: LOG_INFO(" AES-CTR (192-bit);"); break;
        case 0x6U: LOG_INFO(" AES-CTR (256-bit);"); break;
        case 0x7U: LOG_INFO(" Reserved Enc (0x7);"); break;
        default: break;
    }
}

static uint8_t
M17streamDataTypeFromLSF(const struct m17_lsf_result* res) {
    if (res->version == 3U && res->payload_contents == 0xFU) {
        return 20U;
    }
    return res->dt;
}

static void
M17printLICH(const uint8_t* lich_decoded) {
    DSD_FPRINTF(stderr, " LICH: ");
    for (int i = 0; i < 6; i++) {
        DSD_FPRINTF(stderr, "[%02X]", (uint8_t)ConvertBitIntoBytes(&lich_decoded[((size_t)i * 8)], 8));
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

static unsigned long long int
M17readIpSource(const uint8_t* ip_frame) {
    return ((unsigned long long int)ip_frame[4] << 40ULL) + ((unsigned long long int)ip_frame[5] << 32ULL)
           + ((unsigned long long int)ip_frame[6] << 24ULL) + ((unsigned long long int)ip_frame[7] << 16ULL)
           + ((unsigned long long int)ip_frame[8] << 8ULL) + ((unsigned long long int)ip_frame[9] << 0ULL);
}

static void
M17printIpSource(unsigned long long int src) {
    if (src == 0xFFFFFFFFFFFF) {
        DSD_FPRINTF(stderr, "UNKNOWN FFFFFFFFFFFF");
    } else if (src == 0 || src >= 0xEE6B28000000) {
        DSD_FPRINTF(stderr, "RESERVED %012llx", src);
    } else {
        for (int i = 0; i < 9; i++) {
            const char c = m17_base40_alphabet[src % 40];
            DSD_FPRINTF(stderr, "%c", c);
            src = src / 40;
        }
    }
}

static int
M17decodeLICHChunks(const uint8_t* lich_bits, uint8_t* lich_decoded) {
    int err = 0;
    uint8_t lich[4][24];
    DSD_MEMSET(lich, 0, sizeof(lich));

    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 24; j++) {
            lich[i][j] = lich_bits[(i * 24) + j];
        }

        if (!Golay_24_12_decode(lich[i])) {
            err = -1;
        }

        for (int j = 0; j < 12; j++) {
            lich_decoded[i * 12 + j] = lich[i][j];
        }
    }

    return err;
}

static void
M17finalizeLICH(dsd_state* state, const dsd_opts* opts) {
    uint8_t lsf_packed[30];
    DSD_MEMSET(lsf_packed, 0, sizeof(lsf_packed));

    //need to pack bytes for the sw5wwp variant of the crc (might as well, may be useful in the future)
    for (int i = 0; i < 30; i++) {
        lsf_packed[i] = (uint8_t)ConvertBitIntoBytes(&state->m17_lsf[((size_t)i * 8)], 8);
    }

    const uint16_t crc_cmp = crc16m17(lsf_packed, 28);
    const uint16_t crc_ext = (uint16_t)ConvertBitIntoBytes(&state->m17_lsf[224], 16);
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

    if (res.version != 3U && res.rs != 0U) {
        LOG_INFO(" Unknown LSF TYPE;");
        return;
    }

    M17decodeLSFFields(state, &res);
    M17logLSFSummary(state, &res);

    M17storeLSFMeta(state, &res);
    M17decodeLSFMeta(state, &res);
    M17logLSFTrailer(state, &res);
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
}

static void
M17logLSFSummary(dsd_state* state, const struct m17_lsf_result* res) {
    /* Preserve legacy log formatting while routing through LOG_* macros. */
    LOG_INFO("\n");

    LOG_INFO(" CAN: %d", res->cn);
    M17decodeCSD(state, res->dst, res->src);

    if (res->version == 3U) {
        M17logV3PayloadContents(res->payload_contents);
    } else {
        M17logDataType(res->dt);
    }

    if (res->signature != 0U) {
        LOG_INFO(" Signed (secp256r1);");
    } else if (res->version != 3U && res->rs != 0U) {
        LOG_INFO(" RS: %02X", res->rs);
    }
    LOG_INFO("\n");
    if (res->version == 3U) {
        M17logV3Encryption(res->encryption_type);
    } else {
        M17logEncryption(res->et, res->es);
    }
}

static void
M17storeLSFMeta(dsd_state* state, const struct m17_lsf_result* res) {
    //compare incoming META/IV value on AES, if timestamp 32-bits are not within a time 5 minute window, then throw a warning
    // long long int epoch = 1577836800LL;                                     //Jan 1, 2020, 00:00:00 UTC
    // uint32_t tsn = ( (time(NULL)-epoch) & 0xFFFFFFFF); //current LSB 32-bit value
    // uint32_t tsi = (uint32_t)ConvertBitIntoBytes(&state->m17_lsf[112], 32); //OTA LSB 32-bit value
    // uint32_t dif = abs(tsn-tsi);

    //debug

    //pack meta bits into 14 bytes, using state->m17_meta as the AES-IV buffer for M17
    DSD_MEMSET(state->m17_meta, 0, sizeof(state->m17_meta));
    if (res->has_meta != 0U || res->meta_is_iv != 0U) {
        DSD_MEMCPY(state->m17_meta, res->meta, sizeof(res->meta));
    }
}

static void
M17decodeMetaPayload(dsd_state* state, uint8_t identifier) {
    uint8_t meta[15];
    meta[0] = identifier; //add identifier for pkt decoder
    for (int i = 0; i < 14; i++) {
        meta[i + 1] = state->m17_meta[i];
    }
    LOG_INFO("\n ");
    //Note: We don't have opts here, so in the future, if we need it, we will need to pass it here
    decodeM17PKT(NULL, state, meta, 15); //decode META
}

static void
M17decodeLSFMeta(dsd_state* state, const struct m17_lsf_result* res) {
    //Decode Meta Data when not ENC (if meta field is populated with something)
    if (res->version == 3U && res->meta_contents != 0U && res->meta_contents != 0xFU && res->has_meta != 0U) {
        M17decodeMetaPayload(state, (uint8_t)(res->meta_contents + 0x80U));
    } else if (res->version != 3U && res->et == 0U && res->has_meta != 0U) {
        M17decodeMetaPayload(state, (uint8_t)(res->es + 0x80U));
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

    if (res->version == 3U) {
        LOG_INFO("\n FT: %04X; PAY: %X; ENC: %X; SIG: %X; META: %X;", res->type_word, res->payload_contents,
                 res->encryption_type, res->signature, res->meta_contents);
    }
}

static int
M17processLICH(dsd_state* state, const dsd_opts* opts, const uint8_t* lich_bits) {
    uint8_t lich_decoded[48];
    DSD_MEMSET(lich_decoded, 0, sizeof(lich_decoded));

    //execute golay 24,12 or 4 24-bit chunks and reorder into 4 12-bit chunks
    const int err = M17decodeLICHChunks(lich_bits, lich_decoded);

    uint8_t lich_counter = (uint8_t)ConvertBitIntoBytes(&lich_decoded[40], 3);       //lich_cnt
    const uint8_t lich_reserve = (uint8_t)ConvertBitIntoBytes(&lich_decoded[43], 5); //lich_reserved
    (void)lich_reserve; // currently unused; reserved for future use/display

    //sanity check to prevent out of bounds
    if (lich_counter > 5) {
        lich_counter = 5;
    }

    if (err == 0) {
        DSD_FPRINTF(stderr, "LC: %d/6 ", lich_counter + 1);
    } else {
        DSD_FPRINTF(stderr, "LICH G24 ERR");
    }

    //transfer to storage
    for (int i = 0; i < 40; i++) {
        state->m17_lsf[lich_counter * 40 + i] = lich_decoded[i];
    }

    if (opts->payload == 1) {
        M17printLICH(lich_decoded);
    }

    if (lich_counter == 5) {
        M17finalizeLICH(state, opts);
    }

    return err;
}

static void
m17_unpack_voice_octets(const uint8_t* payload, unsigned char* voice1, unsigned char* voice2) {
    for (int i = 0; i < 8; i++) {
        voice1[i] = (unsigned char)ConvertBitIntoBytes(&payload[((size_t)i * 8) + 0], 8);
        voice2[i] = (unsigned char)ConvertBitIntoBytes(&payload[((size_t)i * 8) + 64], 8);
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

#ifdef USE_CODEC2
static int
m17_can_emit_audio(const dsd_opts* opts, const dsd_state* state) {
    return (opts->slot1_on == 1 && state->m17_enc == 0);
}

static void
m17_write_decoded_audio_single(M17_CODEC2_OPTS_PARAM opts, M17_CODEC2_STATE_PARAM state, const short* samples,
                               size_t nsam, const char* log_ctx) {
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
m17_write_decoded_audio_pair(M17_CODEC2_OPTS_PARAM opts, M17_CODEC2_STATE_PARAM state, const short* first,
                             const short* second, size_t nsam, const char* log_ctx) {
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
    sf_write_short(wav_out_f, stereo, ((sf_count_t)count) * 2);
}

static void
m17_maybe_write_wav_single(const dsd_opts* opts, const dsd_state* state, const short* samples, int count) {
    if (opts->wav_out_f == NULL || state->m17_enc != 0) {
        return;
    }

    if (opts->dmr_stereo_wav == 1) {
        sf_write_short(opts->wav_out_f, samples, count);
        return;
    }

    if (opts->static_wav_file == 1) {
        m17_write_static_stereo_wav(samples, count, opts->wav_out_f);
    }
}

static void
m17_maybe_write_wav_pair(const dsd_opts* opts, const dsd_state* state, const short* first, const short* second,
                         int count) {
    if (opts->wav_out_f == NULL || state->m17_enc != 0) {
        return;
    }

    if (opts->dmr_stereo_wav == 1) {
        sf_write_short(opts->wav_out_f, first, count);
        sf_write_short(opts->wav_out_f, second, count);
        return;
    }

    if (opts->static_wav_file == 1) {
        m17_write_static_stereo_wav(first, count, opts->wav_out_f);
        m17_write_static_stereo_wav(second, count, opts->wav_out_f);
    }
}
#endif

static void
M17processCodec2_1600(M17_CODEC2_OPTS_PARAM opts, M17_CODEC2_STATE_PARAM state, const uint8_t* payload) {

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
        adata[i + 1] = (unsigned char)ConvertBitIntoBytes(&payload[((size_t)i * 8) + 64], 8);
    }

    if (m17_any_nonzero_octets(adata + 1, 8)) {
        DSD_FPRINTF(stderr, "\n");           //linebreak
        decodeM17PKT(opts, state, adata, 9); //decode Arbitrary Data as UTF-8
    }
}

static void
M17processCodec2_3200(M17_CODEC2_OPTS_PARAM opts, M17_CODEC2_STATE_PARAM state, const uint8_t* payload) {
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
        DSD_FPRINTF(stderr, "[%02X]", (uint8_t)ConvertBitIntoBytes(&trellis_buf[((size_t)i * 8)], 8));
    }
}

static void
M17printSignatureBits(const uint8_t* trellis_buf) {
    DSD_FPRINTF(stderr, "\n SIG: ");
    for (int i = 2; i < 18; i++) {
        DSD_FPRINTF(stderr, "[%02X]", (uint8_t)ConvertBitIntoBytes(&trellis_buf[((size_t)i * 8)], 8));
    }
}

static void
M17dispatchStreamPayload(M17_CODEC2_OPTS_PARAM opts, M17_CODEC2_STATE_PARAM state, const uint8_t* payload,
                         const uint8_t* trellis_buf, uint16_t frame_number) {
    const int is_signature = m17_stream_frame_is_signature(frame_number);
    if (is_signature != 0 && (state->m17_str_dt == 2U || state->m17_str_dt == 3U)) {
        M17printSignatureBits(trellis_buf);
        return;
    }

    if (state->m17_str_dt == 2) {
        M17processCodec2_3200(opts, state, payload);
    } else if (state->m17_str_dt == 3) {
        M17processCodec2_1600(opts, state, payload);
    } else if (state->m17_str_dt == 1) {
        DSD_FPRINTF(stderr, " DATA;");
    } else if (state->m17_str_dt == 0) {
        DSD_FPRINTF(stderr, "  RES;");
    }

    if (opts->payload == 1 && state->m17_str_dt < 2) {
        M17printStreamBits(trellis_buf);
    }
}

static void
M17prepareStream(M17_CODEC2_OPTS_PARAM opts, dsd_state* state, const uint8_t* m17_bits) {

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
    for (i = 0; i < 18; i++) {
        trellis_buf[((size_t)i * 8) + 0] = (m_data[i] >> 7) & 1;
        trellis_buf[((size_t)i * 8) + 1] = (m_data[i] >> 6) & 1;
        trellis_buf[((size_t)i * 8) + 2] = (m_data[i] >> 5) & 1;
        trellis_buf[((size_t)i * 8) + 3] = (m_data[i] >> 4) & 1;
        trellis_buf[((size_t)i * 8) + 4] = (m_data[i] >> 3) & 1;
        trellis_buf[((size_t)i * 8) + 5] = (m_data[i] >> 2) & 1;
        trellis_buf[((size_t)i * 8) + 6] = (m_data[i] >> 1) & 1;
        trellis_buf[((size_t)i * 8) + 7] = (m_data[i] >> 0) & 1;
    }

    //load m_data into bits for either data packets or voice packets
    uint8_t payload[128];
    DSD_MEMSET(payload, 0, sizeof(payload));

    const uint8_t end = trellis_buf[0];
    const uint16_t fn = (uint16_t)ConvertBitIntoBytes(&trellis_buf[1], 15);

    //insert fn bits into meta 14 and meta 15 for Initialization Vector
    state->m17_meta[14] = (uint8_t)ConvertBitIntoBytes(&trellis_buf[1], 7);
    state->m17_meta[15] = (uint8_t)ConvertBitIntoBytes(&trellis_buf[8], 8);

    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, " FSN: %05d", fn);
    }

    if (end == 1) {
        DSD_FPRINTF(stderr, " END;");
    }

    for (i = 0; i < 128; i++) {
        payload[i] = trellis_buf[i + 16];
    }

    M17dispatchStreamPayload(opts, state, payload, trellis_buf, fn);
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
        dbuf[i] = (uint8_t)getDibit(opts, state);
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
    lich_err = M17processLICH(state, opts, lich_bits);

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
        if (m17_puncture_pattern_1[i % 61] == 1) {
            depunc[i] = soft_bits[bit_index++];
        } else {
            depunc[i] = 0x7FFF;
        }
    }
}

static void
m17_unpack_bytes_to_bits(const uint8_t* bytes, int byte_count, uint8_t* out_bits) {
    if (!bytes || !out_bits || byte_count <= 0) {
        return;
    }
    int bit_index = 0;
    for (int j = 0; j < byte_count; j++) {
        for (int i = 0; i < 8; i++) {
            out_bits[bit_index++] = (bytes[j] >> (7 - i)) & 1;
        }
    }
}

static int
m17_finalize_lsf_crc(const dsd_opts* opts, dsd_state* state, const uint8_t* lsf_packed, uint16_t crc_ext) {
    const uint16_t crc_cmp = crc16m17(lsf_packed, 28);
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
m17_descramble_and_deinterleave_hard(const uint8_t* m17_rnd_bits, uint8_t* m17_bits) {
    uint8_t m17_int_bits[368];
    for (int i = 0; i < 368; i++) {
        m17_int_bits[i] = (m17_rnd_bits[i] ^ m17_scramble[i]) & 1;
    }

    for (int i = 0; i < 368; i++) {
        const int deinterleave_index = ((45 * i) + (92 * i * i)) % 368;
        m17_bits[i] = m17_int_bits[deinterleave_index];
    }
}

static void
m17_depuncture_lsf_debug(const uint8_t* m17_bits, uint8_t* m17_depunc) {
    int bit_in = 0;
    int pattern = 0;
    int out = 0;

    for (int i = 0; i < 488; i++) {
        const int use_history_guess = (i < 48 || i > 96);
        if (m17_puncture_pattern_1[pattern++] == 1) {
            m17_depunc[out++] = m17_bits[bit_in++];
        } else if (use_history_guess && out >= 2 && m17_depunc[out - 2] == 1) {
            m17_depunc[out++] = 1;
        } else {
            m17_depunc[out++] = 0;
        }

        if (pattern == 61) {
            pattern = 0;
        }
    }
}

static void
m17_decode_debug_depunc_to_lsf_bits(const uint8_t* m17_depunc, uint8_t* lsf_bits) {
    uint8_t temp[500];
    uint8_t m_data[32];
    DSD_MEMSET(temp, 0, sizeof(temp));
    DSD_MEMSET(m_data, 0, sizeof(m_data));

    for (int i = 0; i < 488; i++) {
        temp[i] = m17_depunc[i] << 1;
    }

    CNXDNConvolution_start();
    for (int i = 0; i < 244; i++) {
        const uint8_t s0 = temp[((size_t)2 * (size_t)i)];
        const uint8_t s1 = temp[((size_t)2 * (size_t)i) + 1];
        CNXDNConvolution_decode(s0, s1);
    }

    CNXDNConvolution_chainback(m_data, 240);
    m17_unpack_bytes_to_bits(m_data, 30, lsf_bits);
}

//Soft-symbol enhanced version using libM17 Viterbi decoder
void
processM17LSF(dsd_opts* opts, dsd_state* state) {
    float soft_symbols[184];
    uint16_t m17_soft_bits[368];
    uint16_t m17_depunc[488];
    uint8_t lsf_bytes[31];
    uint8_t lsf_packed[30];

    DSD_MEMSET(soft_symbols, 0, sizeof(soft_symbols));
    DSD_MEMSET(m17_soft_bits, 0, sizeof(m17_soft_bits));
    DSD_MEMSET(m17_depunc, 0, sizeof(m17_depunc));
    DSD_MEMSET(lsf_bytes, 0, sizeof(lsf_bytes));
    DSD_MEMSET(lsf_packed, 0, sizeof(lsf_packed));

    m17_capture_soft_symbols(opts, state, soft_symbols);
    m17_soft_bits_from_symbols(soft_symbols, state, m17_soft_bits);
    m17_soft_depuncture_p1(m17_soft_bits, m17_depunc);

    (void)viterbi_decode(lsf_bytes, m17_depunc, 488);
    DSD_MEMCPY(lsf_packed, lsf_bytes + 1, 30);

    DSD_MEMSET(state->m17_lsf, 0, sizeof(state->m17_lsf));
    m17_unpack_bytes_to_bits(lsf_packed, 30, state->m17_lsf);

    const uint16_t crc_ext = (uint16_t)((lsf_packed[28] << 8) + lsf_packed[29]);
    (void)m17_finalize_lsf_crc(opts, state, lsf_packed, crc_ext);

    DSD_FPRINTF(stderr, "\n");
} //end processM17LSF

//original version using nxdn convolutional decoder, used for encoder debug
static void
processM17LSF_debug(const dsd_opts* opts, dsd_state* state, const uint8_t* m17_rnd_bits) {
    uint8_t m17_bits[368];
    uint8_t m17_depunc[500];
    uint8_t lsf_packed[30];

    DSD_MEMSET(m17_bits, 0, sizeof(m17_bits));
    DSD_MEMSET(m17_depunc, 0, sizeof(m17_depunc));
    DSD_MEMSET(lsf_packed, 0, sizeof(lsf_packed));

    m17_descramble_and_deinterleave_hard(m17_rnd_bits, m17_bits);
    m17_depuncture_lsf_debug(m17_bits, m17_depunc);

    DSD_MEMSET(state->m17_lsf, 0, sizeof(state->m17_lsf));
    m17_decode_debug_depunc_to_lsf_bits(m17_depunc, state->m17_lsf);

    for (int i = 0; i < 30; i++) {
        lsf_packed[i] = (uint8_t)ConvertBitIntoBytes(&state->m17_lsf[((size_t)i * 8)], 8);
    }

    const uint16_t crc_ext = (uint16_t)ConvertBitIntoBytes(&state->m17_lsf[224], 16);
    (void)m17_finalize_lsf_crc(opts, state, lsf_packed, crc_ext);
} //end processM17LSF_debug

//debug M17STR for the encoder to pass bits into
static void
processM17STR_debug(M17_CODEC2_OPTS_PARAM opts, dsd_state* state, const uint8_t* m17_rnd_bits) {

    int i;
    uint8_t m17_int_bits[368]; //368 bits that are still interleaved
    uint8_t m17_bits[368];     //368 bits that have been de-interleaved and de-scrambled
    uint8_t lich_bits[96];
    int lich_err = -1;

    DSD_MEMSET(m17_int_bits, 0, sizeof(m17_int_bits));
    DSD_MEMSET(m17_bits, 0, sizeof(m17_bits));
    DSD_MEMSET(lich_bits, 0, sizeof(lich_bits));

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
    lich_err = M17processLICH(state, opts, lich_bits);

    if (lich_err == 0) {
        M17prepareStream(opts, state, m17_bits);
    }

} //end processM17STR_debug

//simple convolutional encoder
static void
simple_conv_encoder(const uint8_t* input, uint8_t* output, int len) {
    int i, k = 0;
    uint8_t d1 = 0;
    uint8_t d2 = 0;
    uint8_t d3 = 0;
    uint8_t d4 = 0;

    for (i = 0; i < len; i++) {
        const uint8_t d = input[i];

        const uint8_t g1 = (d + d3 + d4) & 1;
        const uint8_t g2 = (d + d1 + d2 + d4) & 1;

        d4 = d3;
        d3 = d2;
        d2 = d1;
        d1 = d;

        output[k++] = g1;
        output[k++] = g2;
    }
}

//dibits-symbols map
static const int symbol_map[4] = {+1, +3, -1, -3};

//Sample RRC filter for 48 kHz (alpha=0.5, sps=10, gain=sqrt(sps))
static const float m17_rrc[81] = {
    -0.003195702904062073f, -0.002930279157647190f, -0.001940667871554463f, -0.000356087678023658f,
    0.001547011339077758f,  0.003389554791179751f,  0.004761898604225673f,  0.005310860846138910f,
    0.004824746306020221f,  0.003297923526848786f,  0.000958710871218619f,  -0.001749908029791816f,
    -0.004238694106631223f, -0.005881783042101693f, -0.006150256456781309f, -0.004745376707651645f,
    -0.001704189656473565f, 0.002547854551539951f,  0.007215575568844704f,  0.011231038205363532f,
    0.013421952197060707f,  0.012730475385624438f,  0.008449554307303753f,  0.000436744366018287f,
    -0.010735380379191660f, -0.023726883538258272f, -0.036498030780605324f, -0.046500883189991064f,
    -0.050979050575999614f, -0.047340680079891187f, -0.033554880492651755f, -0.008513823955725943f,
    0.027696543159614194f,  0.073664520037517042f,  0.126689053778116234f,  0.182990955139333916f,
    0.238080025892859704f,  0.287235637987091563f,  0.326040247765297220f,  0.350895727088112619f,
    0.359452932027607974f,  0.350895727088112619f,  0.326040247765297220f,  0.287235637987091563f,
    0.238080025892859704f,  0.182990955139333916f,  0.126689053778116234f,  0.073664520037517042f,
    0.027696543159614194f,  -0.008513823955725943f, -0.033554880492651755f, -0.047340680079891187f,
    -0.050979050575999614f, -0.046500883189991064f, -0.036498030780605324f, -0.023726883538258272f,
    -0.010735380379191660f, 0.000436744366018287f,  0.008449554307303753f,  0.012730475385624438f,
    0.013421952197060707f,  0.011231038205363532f,  0.007215575568844704f,  0.002547854551539951f,
    -0.001704189656473565f, -0.004745376707651645f, -0.006150256456781309f, -0.005881783042101693f,
    -0.004238694106631223f, -0.001749908029791816f, 0.000958710871218619f,  0.003297923526848786f,
    0.004824746306020221f,  0.005310860846138910f,  0.004761898604225673f,  0.003389554791179751f,
    0.001547011339077758f,  -0.000356087678023658f, -0.001940667871554463f, -0.002930279157647190f,
    -0.003195702904062073f};

static float mem[81];
static const uint8_t m17_preamble_a_bits[16] = {0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1};
static const uint8_t m17_preamble_b_bits[16] = {1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0};
static const uint8_t m17_eot_marker_bits[16] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 1, 1, 0, 1};
static const uint8_t m17_lsf_fs_bits[16] = {0, 1, 0, 1, 0, 1, 0, 1, 1, 1, 1, 1, 0, 1, 1, 1};
static const uint8_t m17_str_fs_bits[16] = {1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 1, 1, 1, 0, 1};
static const uint8_t m17_pkt_fs_bits[16] = {0, 1, 1, 1, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1};
static const uint8_t m17_brt_fs_bits[16] = {1, 1, 0, 1, 1, 1, 1, 1, 0, 1, 0, 1, 0, 1, 0, 1};

static uint8_t
m17_pack_dibit_pair(const uint8_t* bits, int pair) {
    return (uint8_t)((bits[pair + 0] << 1) + (bits[pair + 1] << 0));
}

static void
m17_fill_repeating_pattern_dibits(const uint8_t* pattern_bits, uint8_t* output_dibits) {
    for (int i = 0; i < 192; i++) {
        output_dibits[i] = m17_pack_dibit_pair(pattern_bits, ((i * 2) % 16));
    }
}

static void
m17_fill_sync_dibits(const uint8_t* sync_bits, uint8_t* output_dibits) {
    for (int i = 0; i < 8; i++) {
        output_dibits[i] = m17_pack_dibit_pair(sync_bits, (i * 2));
    }
}

static void
m17_apply_frame_prefix_dibits(int type, uint8_t* output_dibits) {
    switch (type) {
        case 11: m17_fill_repeating_pattern_dibits(m17_preamble_a_bits, output_dibits); break;
        case 33: m17_fill_repeating_pattern_dibits(m17_preamble_b_bits, output_dibits); break;
        case 55: m17_fill_repeating_pattern_dibits(m17_eot_marker_bits, output_dibits); break;
        case 1: m17_fill_sync_dibits(m17_lsf_fs_bits, output_dibits); break;
        case 2: m17_fill_sync_dibits(m17_str_fs_bits, output_dibits); break;
        case 3: m17_fill_sync_dibits(m17_brt_fs_bits, output_dibits); break;
        case 4: m17_fill_sync_dibits(m17_pkt_fs_bits, output_dibits); break;
        default: break;
    }
}

static void
m17_load_payload_dibits(const uint8_t* input, uint8_t* output_dibits) {
    for (int i = 0; i < 184; i++) {
        output_dibits[i + 8] = (uint8_t)((input[i * 2 + 0] << 1) + (input[i * 2 + 1] << 0));
    }
}

static void
m17_dibits_to_symbols(const uint8_t* output_dibits, int* output_symbols) {
    for (int i = 0; i < 192; i++) {
        output_symbols[i] = symbol_map[output_dibits[i]];
    }
}

static void
m17_upsample_symbols_10x(const int* output_symbols, int* output_up) {
    for (int i = 0; i < 192; i++) {
        for (int j = 0; j < 10; j++) {
            output_up[((size_t)i * 10) + j] = output_symbols[i];
        }
    }
}

static void
m17_baseband_no_filter(const int* output_up, short* baseband) {
    for (int i = 0; i < 1920; i++) {
        baseband[i] = (short)(output_up[i] * 7168.0f);
    }
}

static void
m17_baseband_rrc_filter(const int* output_symbols, short* baseband) {
    int out = 0;
    for (int i = 0; i < 192; i++) {
        mem[0] = (float)output_symbols[i] * 7168.0f;
        for (int j = 0; j < 10; j++) {
            float mac = 0.0f;
            for (int k = 0; k < 81; k++) {
                mac += mem[k] * m17_rrc[k] * sqrtf(10.0);
            }
            for (int k = 80; k > 0; k--) {
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

static void
m17_maybe_apply_dead_air(int type, uint8_t* output_dibits, short* baseband) {
    if (type == 99) {
        DSD_MEMSET(output_dibits, 0xFF, 192);
        DSD_MEMSET(baseband, 0, 1920 * sizeof(short));
    }
}

static void
m17_write_symbol_capture_if_enabled(dsd_opts* opts, dsd_state* state, const uint8_t* output_dibits,
                                    const int* output_symbols) {
    if (!opts->symbol_out_f) {
        return;
    }
    for (int i = 0; i < 192; i++) {
        write_symbol_capture_record(opts, state, output_dibits[i], (float)output_symbols[i]);
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

    for (int i = 0; i < 192; i++) {
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
        dsd_audio_write(opts->audio_raw_out, baseband, 1920);
        return;
    }

    if (opts->audio_out_type == 8) {
        dsd_udp_audio_hook_blast_analog(opts, state, 1920 * sizeof(short), (short*)baseband);
        return;
    }

    if (opts->audio_out_type == 1) {
        const ssize_t written = dsd_write(opts->audio_out_fd, baseband, 1920 * sizeof(short));
        if (written < 0) {
            LOG_WARN("encodeM17RF: failed to write %zu-byte baseband block", (size_t)(1920 * sizeof(short)));
        }
    }
}

static void
m17_write_baseband_wav_if_enabled(const dsd_opts* opts, const short* baseband) {
    if (opts->wav_out_raw != NULL) {
        sf_write_short(opts->wav_out_raw, baseband, 1920);
        sf_write_sync(opts->wav_out_raw);
    }
}

static void
m17_interleave_368(const uint8_t* in, uint8_t* out) {
    for (int i = 0; i < 368; i++) {
        const int x = ((45 * i) + (92 * i * i)) % 368;
        out[x] = in[i];
    }
}

static void
m17_scramble_368(const uint8_t* in, uint8_t* out) {
    for (int i = 0; i < 368; i++) {
        out[i] = (in[i] ^ m17_scramble[i]) & 1;
    }
}

static void
m17_p2_puncture_368(const uint8_t* encoded, uint8_t* punctured, int* out_k, int* out_x) {
    int k = 0;
    int x = 0;
    for (int i = 0; i < 34; i++) {
        punctured[k++] = encoded[x++];
        punctured[k++] = encoded[x++];
        punctured[k++] = encoded[x++];
        punctured[k++] = encoded[x++];
        punctured[k++] = encoded[x++];
        if (k == 368) {
            break;
        }
        punctured[k++] = encoded[x++];
        punctured[k++] = encoded[x++];
        punctured[k++] = encoded[x++];
        punctured[k++] = encoded[x++];
        punctured[k++] = encoded[x++];
        punctured[k++] = encoded[x++];
        x++;
    }
    *out_k = k;
    *out_x = x;
}

static void
m17_build_brt_frame_bits(const uint8_t* m17_b1, uint8_t* m17_b4s, int* out_k, int* out_x) {
    uint8_t m17_b1c[402];
    uint8_t m17_b2p[368];
    uint8_t m17_b3i[368];
    DSD_MEMSET(m17_b1c, 0, sizeof(m17_b1c));
    DSD_MEMSET(m17_b2p, 0, sizeof(m17_b2p));
    DSD_MEMSET(m17_b3i, 0, sizeof(m17_b3i));
    DSD_MEMSET(m17_b4s, 0, 368);

    simple_conv_encoder(m17_b1, m17_b1c, 201); //197+4
    m17_p2_puncture_368(m17_b1c, m17_b2p, out_k, out_x);
    m17_interleave_368(m17_b2p, m17_b3i);
    m17_scramble_368(m17_b3i, m17_b4s);
}

static void
m17_reverse_brt_bits(const uint8_t* m17_b1, uint8_t* m17_b1r) {
    DSD_MEMSET(m17_b1r, 0, 208);
    for (int i = 0; i < 197; i++) {
        m17_b1r[i + 3] = m17_b1[196 - i];
    }
}

static void
m17_print_brt_sequence(const uint8_t* m17_b1) {
    uint8_t m17_b1r[208];
    m17_reverse_brt_bits(m17_b1, m17_b1r);
    DSD_FPRINTF(stderr, "\n M17 BERT   (ENCODER): ");
    for (int i = 0; i < 25; i++) {
        DSD_FPRINTF(stderr, "%02X", (uint8_t)ConvertBitIntoBytes(&m17_b1r[((size_t)i * 8)], 8));
    }
}

static uint16_t
m17_bert_next_lfsr_bit(uint16_t* lfsr) {
    const uint16_t bit = ((*lfsr >> 8) ^ (*lfsr ^ 4)) & 1;
    *lfsr = (uint16_t)((*lfsr << 1) | bit);
    return bit;
}

static void
m17_shift_bert_sequence(uint8_t* m17_b1, uint16_t bit) {
    for (int j = 1; j < 197; j++) {
        m17_b1[197 - j] = m17_b1[197 - j - 1];
    }
    m17_b1[0] = (uint8_t)bit;
}

//convert bit array into symbols and RF/Audio
static void
encodeM17RF(dsd_opts* opts, dsd_state* state, const uint8_t* input, int type) {
    uint8_t output_dibits[192];
    DSD_MEMSET(output_dibits, 0, sizeof(output_dibits));
    m17_apply_frame_prefix_dibits(type, output_dibits);
    if (type < 5) {
        m17_load_payload_dibits(input, output_dibits);
    }

    int output_symbols[192];
    DSD_MEMSET(output_symbols, 0, sizeof(output_symbols));
    m17_dibits_to_symbols(output_dibits, output_symbols);

    int output_up[192 * 10];
    DSD_MEMSET(output_up, 0, sizeof(output_up));
    m17_upsample_symbols_10x(output_symbols, output_up);

    short baseband[1920];
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

static unsigned long long int
m17_encode_b40_callsign(unsigned long long int value, const char* csd) {
    if (value >= 0xEE6B27FFFFFFULL) {
        return value;
    }
    for (int i = (int)strlen(csd) - 1; i >= 0; i--) {
        for (int j = 0; j < 40; j++) {
            if (csd[i] == m17_base40_alphabet[j]) {
                value = value * 40 + (unsigned long long int)j;
                break;
            }
        }
    }
    return value;
}

static int
m17_open_udp_if_enabled(dsd_opts* opts, dsd_state* state) {
    if (opts->m17_use_ip != 1) {
        return 0;
    }
    const int sock_err = dsd_m17_udp_hook_connect(opts, state);
    if (sock_err < 0) {
        DSD_FPRINTF(stderr, "Error Configuring UDP Socket for M17 IP Frame :( \n");
        opts->m17_use_ip = 0;
        return 0;
    }
    return 1;
}

static void
m17_setup_conn_disc_eotx(unsigned long long int src, uint8_t reflector_module, uint8_t* conn, uint8_t* disc,
                         uint8_t* eotx) {
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

static void
m17_load_lsf_callsigns(uint8_t* m17_lsf, unsigned long long int dst, unsigned long long int src) {
    for (int i = 0; i < 48; i++) {
        m17_lsf[i] = (dst >> (47ULL - (unsigned long long int)i)) & 1;
    }
    for (int i = 0; i < 48; i++) {
        m17_lsf[i + 48] = (src >> (47ULL - (unsigned long long int)i)) & 1;
    }
}

static void
m17_attach_lsf_crc(uint8_t* m17_lsf, uint8_t* lsf_packed, uint16_t* crc_cmp) {
    DSD_MEMSET(lsf_packed, 0, 30);
    for (int i = 0; i < 28; i++) {
        lsf_packed[i] = (uint8_t)ConvertBitIntoBytes(&m17_lsf[((size_t)i * 8)], 8);
    }
    *crc_cmp = crc16m17(lsf_packed, 28);
    for (int i = 0; i < 16; i++) {
        m17_lsf[224 + i] = (*crc_cmp >> (15 - i)) & 1;
    }
}

static void
m17_encode_lsf_for_rf(const uint8_t* m17_lsf, uint8_t* m17_lsfs) {
    uint8_t m17_lsfc[488];
    uint8_t m17_lsfp[368];
    uint8_t m17_lsfi[368];
    DSD_MEMSET(m17_lsfc, 0, sizeof(m17_lsfc));
    DSD_MEMSET(m17_lsfp, 0, sizeof(m17_lsfp));
    DSD_MEMSET(m17_lsfi, 0, sizeof(m17_lsfi));
    DSD_MEMSET(m17_lsfs, 0, 368);

    simple_conv_encoder(m17_lsf, m17_lsfc, 244);
    int x = 0;
    for (int i = 0; i < 488; i++) {
        if (m17_puncture_pattern_1[i % 61] == 1) {
            m17_lsfp[x++] = m17_lsfc[i];
        }
    }
    m17_interleave_368(m17_lsfp, m17_lsfi);
    m17_scramble_368(m17_lsfi, m17_lsfs);
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

static int
m17_str_read_block_pulse(dsd_opts* opts, size_t nsam, int dec, float* sample, short* out, int clip_output) {
    for (size_t i = 0; i < nsam; i++) {
        for (int j = 0; j < dec; j++) {
            short s = 0;
            dsd_audio_read(opts->audio_in_stream, &s, 1);
            *sample = (float)s;
        }
        *sample = m17_scale_input_sample(*sample, opts->input_volume_multiplier);
        out[i] = clip_output ? clip_float_to_short(*sample) : (short)*sample;
    }
    return 1;
}

static int
m17_str_read_block_stdin(dsd_opts* opts, size_t nsam, int dec, float* sample, short* out) {
    int result = 0;
    for (size_t i = 0; i < nsam; i++) {
        for (int j = 0; j < dec; j++) {
            short s = 0;
            result = sf_read_short(opts->audio_in_file, &s, 1);
            *sample = (float)s;
        }
        *sample = m17_scale_input_sample(*sample, opts->input_volume_multiplier);
        out[i] = clip_float_to_short(*sample);
        if (result == 0) {
            sf_close(opts->audio_in_file);
            DSD_FPRINTF(stderr, "Connection to STDIN Disconnected.\n");
            DSD_FPRINTF(stderr, "Closing DSD-neo.\n");
            exitflag = 1;
            break;
        }
    }
    return 1;
}

static int
m17_str_read_block_tcp(dsd_opts* opts, size_t nsam, int dec, float* sample, short* out) {
    int result = 0;
    for (size_t i = 0; i < nsam; i++) {
        for (int j = 0; j < dec; j++) {
            short s = 0;
            result = dsd_net_audio_input_hook_tcp_read_sample(opts->tcp_in_ctx, (int16_t*)&s);
            *sample = (float)s;
        }
        *sample = m17_scale_input_sample(*sample, opts->input_volume_multiplier);
        out[i] = clip_float_to_short(*sample);
        if (result == 0) {
            dsd_net_audio_input_hook_tcp_close(opts->tcp_in_ctx);
            opts->tcp_in_ctx = NULL;
            DSD_FPRINTF(stderr, "Connection to TCP Server Disconnected.\n");
            DSD_FPRINTF(stderr, "Closing DSD-neo.\n");
            exitflag = 1;
            break;
        }
    }
    return 1;
}

static int
m17_str_read_block_udp(dsd_opts* opts, size_t nsam, int dec, float* sample, short* out, int clip_output) {
    int result = 1;
    for (size_t i = 0; i < nsam; i++) {
        for (int j = 0; j < dec; j++) {
            short s = 0;
            if (!dsd_net_audio_input_hook_udp_read_sample(opts, (int16_t*)&s)) {
                result = 0;
            } else {
                *sample = (float)s;
            }
        }
        *sample = m17_scale_input_sample(*sample, opts->input_volume_multiplier);
        out[i] = clip_output ? clip_float_to_short(*sample) : (short)*sample;
        if (result == 0) {
            DSD_FPRINTF(stderr, "UDP input stopped.\n");
            exitflag = 1;
            break;
        }
    }
    return 1;
}

static int
m17_str_read_block_rtl(dsd_opts* opts, dsd_state* state, size_t nsam, int dec, float* sample, short* out,
                       int clip_output) {
#ifdef USE_RADIO
    for (size_t i = 0; i < nsam; i++) {
        for (int j = 0; j < dec; j++) {
            if (!state->rtl_ctx) {
                cleanupAndExit(opts, state);
                return 0;
            }
            int got = 0;
            if (dsd_rtl_stream_io_hook_read(state, sample, 1, &got) < 0 || got != 1) {
                cleanupAndExit(opts, state);
                return 0;
            }
        }
        *sample *= opts->rtl_volume_multiplier;
        out[i] = clip_output ? clip_float_to_short(*sample) : (short)*sample;
    }
    opts->rtl_pwr = dsd_rtl_stream_io_hook_return_pwr(state);
#else
    UNUSED(opts);
    UNUSED(state);
    UNUSED(nsam);
    UNUSED(dec);
    UNUSED(sample);
    UNUSED(out);
    UNUSED(clip_output);
#endif
    return 1;
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
        default: return 1;
    }
}

static int
m17_str_read_audio_inputs(m17_str_ctx* ctx) {
#ifdef USE_RADIO
    if (!m17_str_read_audio_block(ctx, ctx->voice1, 1)) {
        return 0;
    }
#else
    (void)m17_str_read_audio_block(ctx, ctx->voice1, 1);
#endif

    if (ctx->st == 2) {
        const int clip_output = !(ctx->opts->audio_in_type == AUDIO_IN_UDP || ctx->opts->audio_in_type == AUDIO_IN_RTL);
#ifdef USE_RADIO
        if (!m17_str_read_audio_block(ctx, ctx->voice2, clip_output)) {
            return 0;
        }
#else
        (void)m17_str_read_audio_block(ctx, ctx->voice2, clip_output);
#endif
    }
    return 1;
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
    for (int i = 0; i < 16; i++) {
        ctx->state->m17_meta[i] = 0;
    }
}

static void
m17_str_update_input_power(m17_str_ctx* ctx) {
    if (ctx->opts->audio_in_type != 3) {
        ctx->opts->rtl_pwr = raw_pwr(ctx->voice1, ctx->nsam, 1);
        if (ctx->opts->input_warn_db < 0.0) {
            const double db = pwr_to_dB(ctx->opts->rtl_pwr);
            const time_t now = time(NULL);
            if (db <= ctx->opts->input_warn_db
                && (ctx->opts->last_input_warn_time == 0
                    || (int)(now - ctx->opts->last_input_warn_time) >= ctx->opts->input_warn_cooldown_sec)) {
                LOG_WARNING("Input level low (%.1f dBFS). Consider raising sender gain or use --input-volume.\n", db);
                ctx->opts->last_input_warn_time = now;
            }
        }
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
    uint8_t m17_v1[148];
    uint8_t m17_v1c[296];
    uint8_t m17_v1p[272];
    uint8_t m17_l1g[96];
    uint8_t m17_t4c[368];
    uint8_t m17_t4i[368];
    DSD_MEMSET(m17_v1, 0, sizeof(m17_v1));
    DSD_MEMSET(m17_v1c, 0, sizeof(m17_v1c));
    DSD_MEMSET(m17_v1p, 0, sizeof(m17_v1p));
    DSD_MEMSET(m17_l1g, 0, sizeof(m17_l1g));
    DSD_MEMSET(m17_t4c, 0, sizeof(m17_t4c));
    DSD_MEMSET(m17_t4i, 0, sizeof(m17_t4i));
    DSD_MEMSET(frame->m17_t4s, 0, sizeof(frame->m17_t4s));

    m17_str_pack_voice_bits(frame);
    for (int i = 0; i < 64; i++) {
        m17_v1[i + 16] = frame->v1_bits[i];
        m17_v1[i + 80] = frame->v2_bits[i];
    }

    m17_str_update_vox_and_eot(ctx);
    m17_v1[0] = ctx->eot & 1;
    for (int i = 0; i < 15; i++) {
        m17_v1[i + 1] = ((uint8_t)(ctx->fsn >> (14 - i))) & 1;
    }

    simple_conv_encoder(m17_v1, m17_v1c, 148);
    int k = 0;
    int x = 0;
    for (int i = 0; i < 25; i++) {
        m17_v1p[k++] = m17_v1c[x++];
        m17_v1p[k++] = m17_v1c[x++];
        m17_v1p[k++] = m17_v1c[x++];
        m17_v1p[k++] = m17_v1c[x++];
        m17_v1p[k++] = m17_v1c[x++];
        m17_v1p[k++] = m17_v1c[x++];
        m17_v1p[k++] = m17_v1c[x++];
        m17_v1p[k++] = m17_v1c[x++];
        if (k == 272) {
            break;
        }
        m17_v1p[k++] = m17_v1c[x++];
        m17_v1p[k++] = m17_v1c[x++];
        m17_v1p[k++] = m17_v1c[x++];
        x++;
    }
    for (int i = 0; i < 272; i++) {
        m17_t4c[i + 96] = m17_v1p[i];
    }

    for (int i = 0; i < 40; i++) {
        ctx->lsf_chunk[ctx->lich_cnt][i] = ctx->m17_lsf[((ctx->lich_cnt) * 40) + i];
    }
    ctx->lsf_chunk[ctx->lich_cnt][40] = (ctx->lich_cnt >> 2) & 1;
    ctx->lsf_chunk[ctx->lich_cnt][41] = (ctx->lich_cnt >> 1) & 1;
    ctx->lsf_chunk[ctx->lich_cnt][42] = (ctx->lich_cnt >> 0) & 1;

    Golay_24_12_encode(ctx->lsf_chunk[ctx->lich_cnt] + 00, m17_l1g + 00);
    Golay_24_12_encode(ctx->lsf_chunk[ctx->lich_cnt] + 12, m17_l1g + 24);
    Golay_24_12_encode(ctx->lsf_chunk[ctx->lich_cnt] + 24, m17_l1g + 48);
    Golay_24_12_encode(ctx->lsf_chunk[ctx->lich_cnt] + 36, m17_l1g + 72);

    for (int i = 0; i < 96; i++) {
        m17_t4c[i] = m17_l1g[i];
    }
    m17_interleave_368(m17_t4c, m17_t4i);
    m17_scramble_368(m17_t4i, frame->m17_t4s);
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
        ctx->m17_ip_packed[i] = (uint8_t)ConvertBitIntoBytes(&ctx->m17_ip_frame[((size_t)i * 8)], 8);
    }
    const uint16_t ip_crc = crc16m17(ctx->m17_ip_packed, 52);
    for (int i = 0; i < 16; i++) {
        ctx->m17_ip_frame[k++] = (ip_crc >> (15 - i)) & 1;
    }
    for (int i = 52; i < 54; i++) {
        ctx->m17_ip_packed[i] = (uint8_t)ConvertBitIntoBytes(&ctx->m17_ip_frame[((size_t)i * 8)], 8);
    }
}

static void
m17_str_send_lsf_if_needed(m17_str_ctx* ctx) {
    if (ctx->new_lsf != 1) {
        return;
    }

    DSD_FPRINTF(stderr, "\n M17 LSF    (ENCODER): ");
    if (ctx->opts->monitor_input_audio == 0) {
        processM17LSF_debug(ctx->opts, ctx->state, ctx->m17_lsfs);
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
m17_str_print_stream_debug(const m17_str_ctx* ctx, const m17_str_frame_ctx* frame, int udp_gate_on_lich5) {
    DSD_FPRINTF(stderr, "\n M17 Stream (ENCODER): ");
    if (ctx->opts->monitor_input_audio == 0) {
        processM17STR_debug(ctx->opts, ctx->state, frame->m17_t4s);
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
    m17_str_print_stream_debug(ctx, frame, 1);
    encodeM17RF(ctx->opts, ctx->state, frame->m17_t4s, 2);

    m17_str_build_ip_frame(ctx, frame);
    if (ctx->use_ip == 1) {
        (void)dsd_m17_udp_hook_blaster(ctx->opts, ctx->state, 54, ctx->m17_ip_packed);
    }

    ctx->lich_cnt++;
    if (ctx->lich_cnt == 6) {
        ctx->lich_cnt = 0;
    }
    ctx->fsn++;
    if (ctx->fsn > 0x7FFF) {
        ctx->fsn = 0;
    }
}

static void
m17_str_reset_tx_idle_state(m17_str_ctx* ctx) {
    ctx->lich_cnt = 0;
    ctx->fsn = 0;
    ctx->state->carrier = 0;
    ctx->state->synctype = DSD_SYNC_NONE;

    m17_assign_stream_id(ctx->sid);

    m17_attach_lsf_crc(ctx->m17_lsf, ctx->lsf_packed, &ctx->crc_cmp);
    m17_encode_lsf_for_rf(ctx->m17_lsf, ctx->m17_lsfs);
}

static void
m17_str_flush_eot_if_needed(m17_str_ctx* ctx, const m17_str_frame_ctx* frame) {
    if (!(ctx->eot && !ctx->eot_out)) {
        return;
    }

    m17_str_print_stream_debug(ctx, frame, 0);
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
    if (ctx->opts->use_ncurses_terminal == 1) {
        ui_publish_both_and_redraw(ctx->opts, ctx->state);
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

    const uint16_t lsf_fi = M17composeFrameInfo(1, ctx->st, 0, 0, ctx->can, 0);
    for (int i = 0; i < 16; i++) {
        ctx->m17_lsf[96 + i] = (lsf_fi >> (15 - i)) & 1;
    }

    ctx->dst = m17_encode_b40_callsign(ctx->dst, ctx->d40);
    ctx->src = m17_encode_b40_callsign(ctx->src, ctx->s40);

    m17_load_lsf_callsigns(ctx->m17_lsf, ctx->dst, ctx->src);
    m17_attach_lsf_crc(ctx->m17_lsf, ctx->lsf_packed, &ctx->crc_cmp);
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

    DSD_MEMSET(mem, 0, 81 * sizeof(float));
    DSD_MEMSET(ctx->nil, 0, sizeof(ctx->nil));
    opts->frame_m17 = 1;
    state->m17encoder_tx = 1;
    if (opts->use_ncurses_terminal == 1 && state->m17_vox == 0) {
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

    m17_send_dead_air_frames(opts, state, ctx->nil, 25);
    ctx->use_ip = m17_open_udp_if_enabled(opts, state);

    m17_assign_stream_id(ctx->sid);

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
#ifdef USE_RADIO
    if (!m17_str_read_audio_inputs(ctx)) {
        return 0;
    }
#else
    (void)m17_str_read_audio_inputs(ctx);
#endif
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
    return 1;
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
void
encodeM17STR(dsd_opts* opts, dsd_state* state) {
    m17_str_ctx ctx;
    if (!m17_str_init(&ctx, opts, state)) {
        return;
    }

    while (!exitflag) {
#ifdef USE_RADIO
        if (!m17_str_run_iteration(&ctx)) {
            return;
        }
#else
        (void)m17_str_run_iteration(&ctx);
#endif
    }

    m17_str_finalize(&ctx);
}

//encode and create audio of a Project M17 BERT signal
void
encodeM17BRT(dsd_opts* opts, dsd_state* state) {

    //initialize RRC memory buffer
    DSD_MEMSET(mem, 0, 81 * sizeof(float));

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

    //BERT - 197 bits generated from a PRBS9 Generator
    uint8_t m17_b1[201];
    DSD_MEMSET(m17_b1, 0, sizeof(m17_b1));

    uint16_t lfsr = 1; //starting value of the LFSR
    m17_b1[0] = 1;

    while (!exitflag) {
        // Drain UI commands so queued actions take effect during BERT loop
        dsd_runtime_pump_controls(opts, state);

        uint8_t m17_b4s[368];
        int k = 0;
        int x = 0;
        m17_build_brt_frame_bits(m17_b1, m17_b4s, &k, &x);

        //debug K and X bit positions
        DSD_FPRINTF(stderr, " K: %d; X: %d", k, x);
        m17_print_brt_sequence(m17_b1);

        //convert bit array into symbols and RF/Audio
        encodeM17RF(opts, state, m17_b4s, 3);

        const uint16_t bit = m17_bert_next_lfsr_bit(&lfsr);
        m17_shift_bert_sequence(m17_b1, bit);
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
    char text[800];
    uint8_t m17_lsf[240];
    uint8_t lsf_packed[30];
    uint16_t crc_cmp;
    uint8_t m17_lsfs[368];
    uint8_t m17_p1_full[31 * 200];
    uint8_t m17_p1_packed[31 * 25];
    int tlen;
    int block;
    int ptr;
    int pad;
    int lst;
    int x;
    int k;
    uint8_t pbc;
    uint8_t eot;
    int use_ip;
    uint8_t conn[11];
    uint8_t disc[10];
    uint8_t eotx[10];
    uint8_t sid[2];
    uint8_t m17_ip_frame[8000];
    uint8_t m17_ip_packed[25 * 40];
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

    DSD_MEMSET(mem, 0, 81 * sizeof(float));
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
    const uint16_t lsf_fi = M17composeFrameInfo(0, 1, 0, 0, ctx->can, 0);
    for (int i = 0; i < 16; i++) {
        ctx->m17_lsf[96 + i] = (lsf_fi >> (15 - i)) & 1;
    }

    ctx->dst = m17_encode_b40_callsign(ctx->dst, ctx->d40);
    ctx->src = m17_encode_b40_callsign(ctx->src, ctx->s40);
    m17_load_lsf_callsigns(ctx->m17_lsf, ctx->dst, ctx->src);
    m17_attach_lsf_crc(ctx->m17_lsf, ctx->lsf_packed, &ctx->crc_cmp);
    m17_encode_lsf_for_rf(ctx->m17_lsf, ctx->m17_lsfs);
}

static void
m17_pkt_emit_sms_bits(m17_pkt_ctx* ctx) {
    ctx->k = 8;
    for (int i = 0; i < 8; i++) {
        ctx->m17_p1_full[i] = (5 >> (7 - i)) & 1;
    }

    ctx->ptr = 0;
    DSD_FPRINTF(stderr, "\n SMS: ");
    for (int i = 0; i < ctx->tlen; i++) {
        const uint8_t cbyte = (uint8_t)ctx->text[ctx->ptr];
        DSD_FPRINTF(stderr, "%c", cbyte);
        for (int j = 0; j < 8; j++) {
            ctx->m17_p1_full[ctx->k++] = (cbyte >> (7 - j)) & 1;
        }
        if (cbyte == 0) {
            break;
        }
        ctx->ptr++;
        if ((i % 71) == 0 && i != 0) {
            DSD_FPRINTF(stderr, "\n      ");
        }
    }
    DSD_FPRINTF(stderr, "\n");
}

static void
m17_pkt_prepare_payload_layout(m17_pkt_ctx* ctx) {
    ctx->tlen = (int)strlen(ctx->text);
    if ((ctx->tlen % 25) > 23) {
        ctx->tlen += (ctx->tlen % 23) + 1;
    }
    if (ctx->tlen > 771) {
        ctx->tlen = 771;
    }
    ctx->text[ctx->tlen++] = 0x00;
    ctx->text[772] = 0x00;
    m17_pkt_emit_sms_bits(ctx);

    ctx->block = (ctx->ptr / 25) + 1;
    ctx->pad = (ctx->block * 25) - ctx->ptr - 4;
    if (ctx->pad < 1 && ctx->block != 31) {
        ctx->block++;
        ctx->pad = (ctx->block * 25) - ctx->ptr - 4;
    }
    ctx->lst = 23 - ctx->pad + 2;
}

static void
m17_pkt_attach_payload_crc(m17_pkt_ctx* ctx) {
    ctx->x = 0;
    DSD_MEMSET(ctx->m17_p1_packed, 0, sizeof(ctx->m17_p1_packed));
    for (int i = 0; i < 25 * 31; i++) {
        ctx->m17_p1_packed[ctx->x] = (uint8_t)ConvertBitIntoBytes(&ctx->m17_p1_full[((size_t)i * 8)], 8);
        if (ctx->m17_p1_packed[ctx->x] == 0) {
            break;
        }
        ctx->x++;
    }
    ctx->crc_cmp = crc16m17(ctx->m17_p1_packed, ctx->x + 1);
    for (int i = 0; i < 16; i++) {
        ctx->m17_p1_full[ctx->k++] = (ctx->crc_cmp >> (15 - i)) & 1;
    }
}

static void
m17_pkt_print_full_payload(const m17_pkt_ctx* ctx) {
    DSD_FPRINTF(stderr, "\n M17 Packet      FULL: ");
    for (int i = 0; i < 25 * ctx->block; i++) {
        if ((i % 25) == 0 && i != 0) {
            DSD_FPRINTF(stderr, "\n                       ");
        }
        DSD_FPRINTF(stderr, "%02X", (uint8_t)ConvertBitIntoBytes(&ctx->m17_p1_full[((size_t)i * 8)], 8));
    }
    DSD_FPRINTF(stderr, "\n");
}

static void
m17_pkt_send_mpkt_if_enabled(m17_pkt_ctx* ctx) {
    ctx->use_ip = m17_open_udp_if_enabled(ctx->opts, ctx->state);
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

    m17_assign_stream_id(ctx->sid);
    for (int j = 0; j < 2; j++) {
        for (int i = 0; i < 8; i++) {
            ctx->m17_ip_frame[k++] = (ctx->sid[j] >> (7 - i)) & 1;
        }
    }
    for (int i = 0; i < 224; i++) {
        ctx->m17_ip_frame[k++] = ctx->m17_lsf[i];
    }

    for (int i = 0; i < 34; i++) {
        ctx->m17_ip_packed[i] = (uint8_t)ConvertBitIntoBytes(&ctx->m17_ip_frame[((size_t)i * 8)], 8);
    }
    for (int i = 0; i < ctx->x + 1; i++) {
        ctx->m17_ip_packed[i + 34] = (uint8_t)ConvertBitIntoBytes(&ctx->m17_p1_full[((size_t)i * 8)], 8);
    }

    const uint16_t ip_crc = crc16m17(ctx->m17_ip_packed, 34 + 1 + ctx->x);
    uint8_t crc_bits[16];
    DSD_MEMSET(crc_bits, 0, sizeof(crc_bits));
    for (int i = 0; i < 16; i++) {
        crc_bits[i] = (ip_crc >> (15 - i)) & 1;
    }
    for (int i = ctx->x + 34 + 1, j = 0; i < (ctx->x + 34 + 3); i++, j++) {
        ctx->m17_ip_packed[i] = (uint8_t)ConvertBitIntoBytes(&crc_bits[(size_t)j * 8], 8);
    }

    if (ctx->use_ip == 1) {
        const int udp_return = dsd_m17_udp_hook_blaster(ctx->opts, ctx->state, ctx->x + 34 + 3, ctx->m17_ip_packed);
        DSD_FPRINTF(stderr, " UDP IP Frame CRC: %04X; UDP RETURN: %d: X: %d; SENT: %d;", ip_crc, udp_return, ctx->x,
                    ctx->x + 34 + 3);
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
    processM17LSF_debug(ctx->opts, ctx->state, ctx->m17_lsfs);
    DSD_MEMSET(ctx->nil, 0, sizeof(ctx->nil));
    encodeM17RF(ctx->opts, ctx->state, ctx->nil, 11);
    encodeM17RF(ctx->opts, ctx->state, ctx->m17_lsfs, 1);
    *new_lsf = 0;
    ctx->ptr = 0;
}

static void
m17_pkt_send_data_frame(m17_pkt_ctx* ctx) {
    uint8_t m17_p1[210];
    uint8_t m17_p2c[420];
    uint8_t m17_p3p[368];
    uint8_t m17_p4i[368];
    uint8_t m17_p4s[368];
    DSD_MEMSET(m17_p1, 0, sizeof(m17_p1));
    DSD_MEMSET(m17_p2c, 0, sizeof(m17_p2c));
    DSD_MEMSET(m17_p3p, 0, sizeof(m17_p3p));
    DSD_MEMSET(m17_p4i, 0, sizeof(m17_p4i));
    DSD_MEMSET(m17_p4s, 0, sizeof(m17_p4s));

    for (int i = 0; i < 200; i++) {
        m17_p1[i] = ctx->m17_p1_full[ctx->ptr++];
    }
    if (ctx->ptr / 8 >= ctx->block * 25) {
        ctx->eot = 1;
        ctx->pbc = ctx->lst;
    }
    m17_p1[200] = ctx->eot;
    for (int i = 0; i < 5; i++) {
        m17_p1[201 + i] = (ctx->pbc >> (4 - i)) & 1;
    }

    simple_conv_encoder(m17_p1, m17_p2c, 210);
    int x = 0;
    for (int i = 0; i < 420; i++) {
        if (m17_puncture_pattern_3[i % 8] == 1) {
            m17_p3p[x++] = m17_p2c[i];
        }
    }
    m17_interleave_368(m17_p3p, m17_p4i);
    m17_scramble_368(m17_p4i, m17_p4s);

    DSD_FPRINTF(stderr, "\n M17 Packet (ENCODER): ");
    for (int i = 0; i < 26; i++) {
        DSD_FPRINTF(stderr, "%02X", (uint8_t)ConvertBitIntoBytes(&m17_p1[((size_t)i * 8)], 8));
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
void
encodeM17PKT(dsd_opts* opts, dsd_state* state) {
    m17_pkt_ctx ctx;
    m17_pkt_init_defaults(&ctx, opts, state);
    m17_pkt_apply_cli_overrides(&ctx);
    m17_send_dead_air_frames(opts, state, ctx.nil, 25);
    m17_pkt_prepare_lsf(&ctx);
    m17_pkt_prepare_payload_layout(&ctx);
    m17_pkt_attach_payload_crc(&ctx);
    m17_pkt_print_full_payload(&ctx);
    m17_pkt_send_mpkt_if_enabled(&ctx);
    m17_pkt_run_loop(&ctx);
}

static void
m17_decode_pkt_print_callsign9(unsigned long long int v) {
    for (int i = 0; i < 9; i++) {
        const char c = m17_base40_alphabet[v % 40];
        DSD_FPRINTF(stderr, "%c", c);
        v = v / 40;
    }
}

static void
m17_decode_pkt_print_text(const char* label, const uint8_t* input, int len) {
    DSD_FPRINTF(stderr, "%s", label);
    for (int i = 1; i < len; i++) {
        DSD_FPRINTF(stderr, "%c", input[i]);
        if ((i % 71) == 0) {
            DSD_FPRINTF(stderr, "\n      ");
        }
    }
}

static void
m17_decode_pkt_print_sms(const uint8_t* input, int len) {
    m17_decode_pkt_print_text("\n SMS: ", input, len);
}

static void
m17_decode_pkt_print_tle(const uint8_t* input, int len) {
    m17_decode_pkt_print_text(" TLE:\n", input, len);
}

static void
m17_decode_pkt_print_extended_csd(const uint8_t* input) {
    const unsigned long long int src =
        ((unsigned long long int)input[1] << 40ULL) + ((unsigned long long int)input[2] << 32ULL)
        + ((unsigned long long int)input[3] << 24ULL) + ((unsigned long long int)input[4] << 16ULL)
        + ((unsigned long long int)input[5] << 8ULL) + ((unsigned long long int)input[6] << 0ULL);
    const unsigned long long int dst =
        ((unsigned long long int)input[7] << 40ULL) + ((unsigned long long int)input[8] << 32ULL)
        + ((unsigned long long int)input[9] << 24ULL) + ((unsigned long long int)input[10] << 16ULL)
        + ((unsigned long long int)input[11] << 8ULL) + ((unsigned long long int)input[12] << 0ULL);

    DSD_FPRINTF(stderr, " CF1: ");
    m17_decode_pkt_print_callsign9(src);
    if (dst != 0ULL) {
        DSD_FPRINTF(stderr, " REF: ");
        m17_decode_pkt_print_callsign9(dst);
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
    if (gnss.reserved != 0U) {
        DSD_FPRINTF(stderr, " Reserved: %03X;", gnss.reserved);
    }

    m17_decode_pkt_print_gnss_source(gnss.data_source);
    m17_decode_pkt_print_station_type(gnss.station_type);
}

static void
m17_decode_pkt_print_meta_or_arb(const uint8_t* input, int len, uint8_t protocol) {
    DSD_FPRINTF(stderr, " ");
    if (protocol == 0x80 || protocol == 0x83) {
        uint8_t segment_num = 1U;
        uint8_t segment_len = 1U;
        (void)m17_meta_text_segment_info(protocol, input[1], &segment_num, &segment_len);
        DSD_FPRINTF(stderr, "%d/%d; ", segment_num, segment_len);
        for (int i = 2; i < len; i++) {
            DSD_FPRINTF(stderr, "%c", input[i]);
        }
        return;
    }

    for (int i = 1; i < len; i++) {
        DSD_FPRINTF(stderr, "%c", input[i]);
    }
}

static void
m17_decode_pkt_print_hex(const uint8_t* input, int len) {
    DSD_FPRINTF(stderr, " ");
    for (int i = 1; i < len; i++) {
        DSD_FPRINTF(stderr, "%02X", input[i]);
    }
}

static void
decodeM17PKT(const dsd_opts* opts, const dsd_state* state, const uint8_t* input, int len) {
    //Decode the completed packet
    UNUSED(opts);
    const uint8_t protocol = input[0];
    const char* protocol_name = m17_packet_protocol_name(protocol);
    DSD_FPRINTF(stderr, " Protocol:");
    if (protocol_name != NULL) {
        DSD_FPRINTF(stderr, " %s;", protocol_name);
    } else {
        DSD_FPRINTF(stderr, " Res/Unk: %02X;", protocol);
    }

    //check for encryption, if encrypted, skip decode and report as encrypted
    if (state->m17_enc != 0 && protocol != 0x69 && (protocol < 0x80 || protocol > 0x83)) {
        DSD_FPRINTF(stderr, " *Encrypted*");
        return;
    }

    switch (protocol) {
        case 0x05: m17_decode_pkt_print_sms(input, len); break;
        case 0x07: m17_decode_pkt_print_tle(input, len); break;
        case 0x82: m17_decode_pkt_print_extended_csd(input); break;
        case 0x81:
        case 0x91: m17_decode_pkt_print_gnss(input, len); break;
        case 0x80:
        case 0x83:
        case 0x89:
        case 0x99: m17_decode_pkt_print_meta_or_arb(input, len, protocol); break;
        default: m17_decode_pkt_print_hex(input, len); break;
    }
}

static void
m17_soft_depuncture_p3(const uint16_t* soft_bits, uint16_t* depunc) {
    int bit_index = 0;
    for (int i = 0; i < 420; i++) {
        if (m17_puncture_pattern_3[i % 8] == 1) {
            depunc[i] = soft_bits[bit_index++];
        } else {
            depunc[i] = 0x7FFF;
        }
    }
}

static int
m17_pkt_ptr_clamped(int pbc_count) {
    int ptr = pbc_count * 25;
    if (ptr > 825) {
        return 825;
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
        if ((i % 25) == 0 && i != 0) {
            DSD_FPRINTF(stderr, "\n     ");
        }
        DSD_FPRINTF(stderr, " %02X", state->m17_pkt[i]);
    }
    DSD_FPRINTF(stderr, "\n      CRC - C: %04X; E: %04X", crc_cmp, crc_ext);
}

static void
m17_pkt_finalize_eot(const dsd_opts* opts, dsd_state* state, int total, int end) {
    if (total < 0) {
        total = 0;
    }
    const int max_total = (int)sizeof(state->m17_pkt) - 3;
    if (total > max_total) {
        total = max_total;
    }
    const uint16_t crc_cmp = crc16m17(state->m17_pkt, total + 1);
    const uint16_t crc_ext = (state->m17_pkt[total + 1] << 8) + state->m17_pkt[total + 2];

    if (crc_cmp == crc_ext || opts->aggressive_framesync == 0) {
        decodeM17PKT(opts, state, state->m17_pkt, total);
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

    float soft_symbols[184];     //Raw float symbol values for soft-decision Viterbi
    uint16_t m17_soft_bits[368]; //368 soft costs (de-interleaved and de-scrambled)
    uint16_t m17_depunc[488];    //488 weighted values after depuncturing
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
    (void)viterbi_decode(pkt_bytes, m17_depunc, 420);
    DSD_MEMCPY(pkt_packed, pkt_bytes + 1, 26);

    const uint8_t counter = (pkt_packed[25] >> 2) & 0x1F;
    const uint8_t eot = (pkt_packed[25] >> 7) & 1;

    const int ptr = m17_pkt_ptr_clamped(state->m17_pbc_ct);

    int total = ptr + counter - 3; //-3 if changes to M17_Implementations are made
    if (total < 0 && eot == 1) {
        total = 0;
    }

    const int end = ptr + 25;
    m17_pkt_log_counter(state->m17_pbc_ct, counter, eot);
    DSD_MEMCPY(state->m17_pkt + ptr, pkt_packed, 25);
    m17_pkt_log_frame_if_enabled(opts, pkt_packed);

    if (eot) {
        m17_pkt_finalize_eot(opts, state, total, end);
    }

    if (!eot) {
        state->m17_pbc_ct++;
    }

    //ending linebreak
    DSD_FPRINTF(stderr, "\n");

} //end processM17PKT

static const uint8_t m17_ip_magic[4] = {0x4D, 0x31, 0x37, 0x20};
static const uint8_t m17_ip_ackn[4] = {0x41, 0x43, 0x4B, 0x4E};
static const uint8_t m17_ip_nack[4] = {0x4E, 0x41, 0x43, 0x4B};
static const uint8_t m17_ip_conn[4] = {0x43, 0x4F, 0x4E, 0x4E};
static const uint8_t m17_ip_disc[4] = {0x44, 0x49, 0x53, 0x43};
static const uint8_t m17_ip_ping[4] = {0x50, 0x49, 0x4E, 0x47};
static const uint8_t m17_ip_pong[4] = {0x50, 0x4F, 0x4E, 0x47};
static const uint8_t m17_ip_eotx[4] = {0x45, 0x4F, 0x54, 0x58};
static const uint8_t m17_ip_mpkt[4] = {0x4D, 0x50, 0x4B, 0x54};

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

    const uint16_t sid = (uint16_t)ConvertBitIntoBytes(&ip_bits[32], 16);
    m17_ip_copy_lsf_from_bits(state, ip_bits);

    const uint16_t fn = (uint16_t)ConvertBitIntoBytes(&ip_bits[273], 15);
    const uint8_t eot = ip_bits[272];
    state->m17_meta[14] = (uint16_t)ConvertBitIntoBytes(&ip_bits[273], 7);
    state->m17_meta[15] = (uint16_t)ConvertBitIntoBytes(&ip_bits[280], 8);
    DSD_FPRINTF(stderr, "\n M17 IP Stream: %04X; FN: %05d;", sid, fn);
    if (eot) {
        DSD_FPRINTF(stderr, " EOT;");
    }

    for (int i = 0; i < 128; i++) {
        payload[i] = ip_bits[i + 288];
    }

    const uint16_t crc_ext = (uint16_t)((ip_frame[52] << 8) + ip_frame[53]);
    const uint16_t crc_cmp = crc16m17(ip_frame, 52);
    if (crc_ext == crc_cmp) {
        M17decodeLSF(state);
    }
    if (state->m17_str_dt == 2) {
        M17processCodec2_3200((dsd_opts*)opts, state, payload);
    } else if (state->m17_str_dt == 3) {
        M17processCodec2_1600((dsd_opts*)opts, state, payload);
    }

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
        M17printIpSource(M17readIpSource(ip_frame));
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
    const uint16_t sid = (uint16_t)ConvertBitIntoBytes(&ip_bits[32], 16);
    m17_ip_copy_lsf_from_bits(state, ip_bits);

    const uint16_t crc_ext = (uint16_t)((ip_frame[err - 2] << 8) + ip_frame[err - 1]);
    const uint16_t crc_cmp = crc16m17(ip_frame, (uint16_t)(err - 2));
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

static void
m17_ip_dispatch_frame(const dsd_opts* opts, dsd_state* state, const uint8_t* ip_frame, int err) {
    if (memcmp(ip_frame, m17_ip_magic, 4) == 0) {
        m17_ip_handle_stream_frame(opts, state, ip_frame);
        return;
    }

    if (memcmp(ip_frame, m17_ip_mpkt, 4) == 0) {
        m17_ip_handle_mpkt_frame(opts, state, ip_frame, err);
        return;
    }

    static const m17_ip_ctrl_desc ctrl[] = {
        {m17_ip_ackn, "ACNK", 0, 0, 0, 0},  {m17_ip_nack, "NACK", 0, 0, 0, 0},  {m17_ip_conn, "CONN", 11, 1, 1, 0},
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
void
processM17IPF(dsd_opts* opts, dsd_state* state) {
    opts->dmr_stereo = 0;
    opts->audio_in_type = AUDIO_IN_NULL;
    opts->udp_sockfd = dsd_m17_udp_hook_udp_bind(opts->m17_hostname, opts->m17_portno);

    int err = 1;
    uint8_t ip_frame[1000];
    DSD_MEMSET(ip_frame, 0, sizeof(ip_frame));

    while (!exitflag) {
        dsd_runtime_pump_controls(opts, state);

        if (opts->udp_sockfd) {
            err = dsd_m17_udp_hook_receiver(opts, &ip_frame);
        } else {
            exitflag = 1;
        }

        m17_ip_dispatch_frame(opts, state, ip_frame, err);

        if (opts->use_ncurses_terminal == 1) {
            ui_publish_both_and_redraw(opts, state);
        }
        watchdog_event_history(opts, state, 0);
        watchdog_event_current(opts, state, 0);
        DSD_MEMSET(ip_frame, 0, sizeof(ip_frame));
    }
}
