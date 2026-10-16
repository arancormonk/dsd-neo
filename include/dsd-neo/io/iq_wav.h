// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Two-channel WAV I/Q recordings (RIFF, RF64 and BW64), as other SDR software writes them.
 *
 * The parser reads the container's header and chunk list and describes where the I/Q frames are; the replay reader
 * (<dsd-neo/io/iq_replay.h>) converts those frames to cf32 as it reads them. Channel 1 is I and channel 2 is Q.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_IO_IQ_WAV_H_
#define DSD_NEO_INCLUDE_DSD_NEO_IO_IQ_WAV_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Most chunks the parser walks before it gives up on a file. */
#define DSD_IQ_WAV_MAX_CHUNKS     4096U

/** Bytes dsd_iq_wav_sniff() needs to tell a WAV header from anything else. */
#define DSD_IQ_WAV_SNIFF_BYTES    12U

/** Largest magnitude a 64-bit float sample keeps; larger ones and infinities are clamped to it, NaN becomes 0. */
#define DSD_IQ_WAV_F64_CLAMP      1000000.0

/** Room for a start time in the YYYY-MM-DDTHH:MM:SSZ form, with its terminator. */
#define DSD_IQ_WAV_START_UTC_SIZE 24U

typedef enum {
    DSD_IQ_WAV_CONTAINER_RIFF = 0, /**< RIFF/WAVE, 32-bit sizes */
    DSD_IQ_WAV_CONTAINER_RF64 = 1, /**< RF64/WAVE (EBU Tech 3306), 64-bit sizes in ds64 */
    DSD_IQ_WAV_CONTAINER_BW64 = 2, /**< BW64/WAVE (ITU-R BS.2088), laid out as RF64 */
} dsd_iq_wav_container;

typedef enum {
    DSD_IQ_WAV_ENC_U8 = 0,  /**< 8-bit unsigned PCM, centred on 127.5 */
    DSD_IQ_WAV_ENC_S16 = 1, /**< 16-bit signed PCM */
    DSD_IQ_WAV_ENC_S24 = 2, /**< 24-bit signed PCM, packed in 3 bytes */
    DSD_IQ_WAV_ENC_S32 = 3, /**< 32-bit signed PCM */
    DSD_IQ_WAV_ENC_F32 = 4, /**< 32-bit IEEE float */
    DSD_IQ_WAV_ENC_F64 = 5, /**< 64-bit IEEE float */
} dsd_iq_wav_encoding;

/** Where a recording's centre frequency or start time came from. */
typedef enum {
    DSD_IQ_WAV_FROM_NONE = 0,      /**< not recorded */
    DSD_IQ_WAV_FROM_AUXI = 1,      /**< the auxi chunk (SDR#, HDSDR, SDRuno) */
    DSD_IQ_WAV_FROM_FILE_NAME = 2, /**< the file name's tokens */
} dsd_iq_wav_source;

typedef enum {
    DSD_IQ_WAV_SNIFF_NOT_WAV = 0, /**< anything else */
    DSD_IQ_WAV_SNIFF_WAV = 1,     /**< RIFF, RF64 or BW64 followed by WAVE */
    DSD_IQ_WAV_SNIFF_RIFX = 2,    /**< big-endian RIFX/WAVE, which the parser refuses */
} dsd_iq_wav_sniff_result;

typedef struct {
    dsd_iq_wav_container container;
    dsd_iq_wav_encoding encoding;
    uint16_t format_tag;      /**< 1 PCM, 3 IEEE float, 0xFFFE EXTENSIBLE */
    uint16_t bits_per_sample; /**< container bits of one channel's sample */
    uint16_t valid_bits;      /**< EXTENSIBLE's valid bits, else bits_per_sample */
    uint16_t block_align;     /**< bytes of one I/Q frame: 2 x bits_per_sample / 8 */
    uint32_t sample_rate_hz;
    uint64_t file_size;            /**< the file's size when it was read */
    uint64_t data_offset;          /**< where the first frame starts */
    uint64_t declared_data_bytes;  /**< the data chunk's size as the header gives it (0 or 0xFFFFFFFF if unfinalized) */
    uint64_t effective_data_bytes; /**< the whole frames the file holds, in bytes */
    uint64_t frames;               /**< effective_data_bytes / block_align */
    int truncated;                 /**< the header declares more data than the file holds */
    int unfinalized;               /**< the header was never finalized: the data runs to the end of the file */
    uint64_t container_end;        /**< where the container ends (8 + its RIFF or ds64 size); UINT64_MAX when the
                                        container runs to the end of the file (an unfinalized RIFF, or a RIFF size of 0,
                                        0xFFFFFFFF or past the file) */
    int container_clamped;         /**< the data chunk declares more than its container holds: a writer inconsistency;
                                        the data stops at the container's end */
    uint64_t center_frequency_hz;  /**< 0 when unknown */
    dsd_iq_wav_source center_source;
    char start_utc[DSD_IQ_WAV_START_UTC_SIZE]; /**< YYYY-MM-DDTHH:MM:SSZ, empty when unknown */
    dsd_iq_wav_source start_source;
} dsd_iq_wav_info;

/**
 * @brief Classify a file from its first bytes.
 *
 * @param head The file's first @p len bytes.
 * @param len  At least DSD_IQ_WAV_SNIFF_BYTES for anything but DSD_IQ_WAV_SNIFF_NOT_WAV.
 * @return A dsd_iq_wav_sniff_result.
 */
int dsd_iq_wav_sniff(const void* head, size_t len);

/**
 * @brief Read a WAV I/Q recording's header and chunk list.
 *
 * Accepts RIFF, RF64 and BW64 containers holding exactly 2 channels of 8-bit unsigned, 16/24/32-bit signed PCM, or
 * 32/64-bit IEEE float, as plain or EXTENSIBLE fmt chunks. The centre frequency comes from an auxi chunk, else from
 * the file name (dsd_iq_wav_center_from_name()); the start time likewise (dsd_iq_wav_start_from_name()). A header
 * that was never finalized (a RIFF whose data size is 0 or 0xFFFFFFFF in a container whose size is 0, 0xFFFFFFFF or
 * past the file, with data as its last chunk) runs to the end of the file. Otherwise the data stops at the
 * container's end when it declares more than that (container_clamped, a writer inconsistency). A data chunk that
 * declares nothing is refused.
 *
 * @param path         The file.
 * @param out          [out] Set only on success.
 * @param err_buf      Receives a message naming @p path on failure. May be NULL.
 * @param err_buf_size Size of @p err_buf.
 * @return DSD_IQ_OK, DSD_IQ_ERR_IO, DSD_IQ_ERR_INVALID_META, DSD_IQ_ERR_UNSUPPORTED_FMT or DSD_IQ_ERR_INVALID_ARG.
 */
int dsd_iq_wav_read_info(const char* path, dsd_iq_wav_info* out, char* err_buf, size_t err_buf_size);

/**
 * @brief The data bytes a recording holds for a file of @p file_size bytes, whole frames or not.
 *
 * The declared data size, cut at the container's end (dsd_iq_wav_info::container_end) and at the end of the file. An
 * unfinalized header runs to the end of the file.
 *
 * @param info          A dsd_iq_wav_read_info() result.
 * @param file_size     The file's size now.
 * @param out_truncated [out] Set when the file ends before that data does. May be NULL.
 * @return The bytes, 0 for a NULL @p info.
 */
uint64_t dsd_iq_wav_data_bytes_in_file(const dsd_iq_wav_info* info, uint64_t file_size, int* out_truncated);

/**
 * @brief The whole frames a recording holds for a file of @p file_size bytes (dsd_iq_wav_data_bytes_in_file()).
 *
 * @param info          A dsd_iq_wav_read_info() result.
 * @param file_size     The file's size now.
 * @param out_frames    [out] The whole frames.
 * @param out_truncated [out] Set when the file ends before the data the header gives does. May be NULL.
 * @return DSD_IQ_OK or DSD_IQ_ERR_INVALID_ARG.
 */
int dsd_iq_wav_frames_in_file(const dsd_iq_wav_info* info, uint64_t file_size, uint64_t* out_frames,
                              int* out_truncated);

/**
 * @brief The centre frequency a file name records.
 *
 * Takes the last '_'-separated token of the base name (without a ".wav" extension) that reads as
 * `<n>[.<f>](Hz|kHz|MHz)` and gives a whole number of hertz, as SDR#, SDR++ and SDRuno name their recordings. A value
 * of 0 or above UINT32_MAX counts as unknown.
 *
 * @return 1 with *@p out_hz set, or 0 when the name records none.
 */
int dsd_iq_wav_center_from_name(const char* path, uint64_t* out_hz);

/**
 * @brief The start time a file name records, from SDR#/SDRuno's `YYYYMMDD_HHMMSSZ` tokens (UTC).
 *
 * @param out      [out] YYYY-MM-DDTHH:MM:SSZ.
 * @param out_size At least DSD_IQ_WAV_START_UTC_SIZE.
 * @return 1 with @p out set, or 0 when the name records no valid time.
 */
int dsd_iq_wav_start_from_name(const char* path, char* out, size_t out_size);

/**
 * @brief Convert @p frames interleaved I/Q frames of @p encoding to cf32.
 *
 * u8 becomes (x - 127.5) / 127.5, as a cu8 capture widens; signed PCM is divided by its full scale; f32 is copied; f64
 * is classified before it is narrowed: NaN becomes 0, and infinities and magnitudes above DSD_IQ_WAV_F64_CLAMP are
 * clamped to it.
 *
 * @param src Little-endian frames, @p frames x the encoding's frame bytes.
 * @param dst 2 x @p frames floats.
 */
void dsd_iq_wav_frames_to_cf32(dsd_iq_wav_encoding encoding, const void* src, size_t frames, float* dst);

/** @brief "PCM 8-bit unsigned", "PCM 16-bit", ..., "IEEE float 64-bit". */
const char* dsd_iq_wav_encoding_name(dsd_iq_wav_encoding encoding);

/** @brief "RIFF", "RF64" or "BW64". */
const char* dsd_iq_wav_container_name(dsd_iq_wav_container container);

/** @brief "auxi chunk", "file name" or "none". */
const char* dsd_iq_wav_source_name(dsd_iq_wav_source source);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_IO_IQ_WAV_H_ */
