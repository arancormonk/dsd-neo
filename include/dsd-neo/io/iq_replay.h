// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief IQ replay metadata and source reader API.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_IO_IQ_REPLAY_H_
#define DSD_NEO_INCLUDE_DSD_NEO_IO_IQ_REPLAY_H_

#include <dsd-neo/io/iq_types.h>
#include <dsd-neo/io/iq_wav.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Largest base decimation a replay chain may request. The demodulator keeps one half-band
 * history buffer per pass (demod_state::hb_hist_i), so 2^10 is the hard ceiling. */
#define DSD_IQ_REPLAY_MAX_BASE_DECIMATION     1024U

/* A replay hands the demodulator one capture chunk per block (issue #572), so the chunk is the replay's demod block. It
 * is at most one live RTL-SDR transfer, DEFAULT_BUF_LENGTH bytes of cu8 (8192 complex samples), and no longer than such
 * a transfer lasts at the default 1.536 Msps capture rate (16/3 ms): every per-block decision (the level squelch, the
 * squelch envelope, the published channel power, adaptive loop gains, metrics, the input level) then runs at a live
 * receiver's cadence (issue #626). */
#define DSD_IQ_REPLAY_CHUNK_MAX_SAMPLES       8192U
#define DSD_IQ_REPLAY_CHUNK_REFERENCE_RATE_HZ 1536000U

/* Room for dsd_iq_replay_config::open_notice. */
#define DSD_IQ_REPLAY_NOTICE_SIZE             1024U

/** What a replay reads its capture from. */
typedef enum {
    DSD_IQ_REPLAY_CONTAINER_SIDECAR = 0, /**< raw samples described by a dsd-neo-iq JSON sidecar */
    DSD_IQ_REPLAY_CONTAINER_WAV = 1,     /**< a 2-channel WAV I/Q recording, read as virtual cf32 */
} dsd_iq_replay_container;

typedef struct {
    uint32_t metadata_version;
    char data_path[2048];
    char metadata_path[2048];
    dsd_iq_sample_format format;
    char capture_stage[64];
    uint32_t sample_rate_hz;
    uint64_t center_frequency_hz;
    uint64_t capture_center_frequency_hz;
    uint64_t data_bytes;
    int ppm;
    int tuner_gain_tenth_db;
    int rtl_dsp_bw_khz;
    uint32_t base_decimation;
    uint32_t post_downsample;
    uint32_t demod_rate_hz;
    int offset_tuning_enabled;
    int fs4_shift_enabled;
    /* Derived from persisted combine_rotate_enabled=false metadata. */
    int historical_cu8_two_pass;
    int muted_bytes_excluded;
    int contains_retunes;
    uint32_t capture_retune_count;
    uint64_t capture_drops;
    uint64_t capture_drop_blocks;
    uint64_t input_ring_drops;
    /* Capture stopped because it hit --iq-capture-max-mb rather than being cut
     * short. Absent from metadata written before this field existed, in which
     * case it reads back as 0. */
    int size_limit_reached;
    char source_backend[32];
    char source_args[256];
    char capture_started_utc[64];
    char notes[256];
    uint32_t event_count;
    dsd_iq_event* events;
    int loop;
    int realtime;
    /* A WAV recording (issue #701) replays as a virtual cf32 capture at stage post_driver_cf32_pre_ring: format,
     * sample_rate_hz and the centre come from its header, data_bytes counts its frames as 8-byte cf32 frames (the
     * reader converts as it reads), and event and byte offsets are in those virtual bytes. data_path names the WAV;
     * metadata_path names the path the replay was opened by (the WAV, or a sidecar whose data_file is the WAV). */
    dsd_iq_replay_container container;
    dsd_iq_wav_info wav; /* container == DSD_IQ_REPLAY_CONTAINER_WAV */
    /* The complex rate the demodulator takes. 0 means sample_rate_hz, the capture's own rate. */
    uint32_t dsp_input_rate_hz;
    /* The DSP bandwidth the capture recorded (the sidecar's rtl_dsp_bw_khz), as read; 0 for a WAV. */
    int recorded_dsp_bw_khz;
    /* Something the open wants the user told (a sidecar beside a WAV that the WAV's header overrides), or empty. */
    char open_notice[DSD_IQ_REPLAY_NOTICE_SIZE];
} dsd_iq_replay_config;

typedef struct dsd_iq_replay_source dsd_iq_replay_source;

/**
 * @brief Parse replay metadata without opening a data stream.
 *
 * @p path is a sidecar, a raw data file with a sidecar beside it, or a WAV I/Q recording. A WAV is recognised by its
 * header before any sidecar is read, and a sidecar whose data_file is a WAV is ignored apart from that name (its other
 * fields are not validated): the WAV's header describes the capture, and open_notice says the sidecar was ignored.
 *
 * On success, overwrites @p out_cfg without inspecting its prior contents.
 * Call dsd_iq_replay_config_clear() when done with a successful result. If
 * reusing a config that already owns events, clear it before the next read.
 */
int dsd_iq_replay_read_metadata(const char* path, dsd_iq_replay_config* out_cfg, char* err_buf, size_t err_buf_size);

/**
 * @brief Parse replay metadata and optionally open the data stream.
 *
 * If @p out is NULL, metadata-only validation is performed.
 * On success, overwrites @p out_cfg without inspecting its prior contents.
 * Call dsd_iq_replay_config_clear() when done with a successful result. If
 * reusing a config that already owns events, clear it before the next open.
 */
int dsd_iq_replay_open(const char* path, dsd_iq_replay_config* out_cfg, dsd_iq_replay_source** out, char* err_buf,
                       size_t err_buf_size);
/**
 * @brief Seconds since the Unix epoch of a sidecar's `capture_started_utc`.
 *
 * Takes the form the capture writer stamps, `YYYY-MM-DDTHH:MM:SSZ`, and nothing looser. An I/Q replay decodes on the
 * capture's clock from this instant (issue #572).
 *
 * @param text The field's text.
 * @param out_s [out] The instant; set only on success.
 * @return DSD_IQ_OK; DSD_IQ_ERR_INVALID_META for text in any other form or out of range; DSD_IQ_ERR_INVALID_ARG for a
 *         NULL argument.
 */
int dsd_iq_replay_parse_utc_seconds(const char* text, int64_t* out_s);

/**
 * @brief Release owned replay config allocations and reset the struct to zero.
 *
 * Only call this on a zero-initialized config or a config returned by a
 * successful replay metadata/open call.
 */
void dsd_iq_replay_config_clear(dsd_iq_replay_config* cfg);

/**
 * @brief Read up to @p max_bytes of the capture's data into @p out.
 *
 * @p out_bytes gets the bytes read, 0 with DSD_IQ_OK once every byte the open measured for the capture is read.
 * DSD_IQ_ERR_IO for a failed read, and for a data file that ends before those bytes (cut short after the open).
 */
int dsd_iq_replay_read(dsd_iq_replay_source* src, void* out, size_t max_bytes, size_t* out_bytes);
int dsd_iq_replay_rewind(dsd_iq_replay_source* src);

/**
 * @brief Move the reader to @p capture_byte_offset, rounded down to a whole complex sample.
 *
 * The offset is in capture bytes: the data file's bytes for a sidecar capture, the virtual cf32 bytes for a WAV (8 per
 * frame, as data_bytes counts them). The next read starts there.
 *
 * @return DSD_IQ_OK; DSD_IQ_ERR_INVALID_ARG for an offset past the capture's measured bytes or a NULL source;
 *         DSD_IQ_ERR_IO when the file cannot seek.
 */
int dsd_iq_replay_seek(dsd_iq_replay_source* src, uint64_t capture_byte_offset);
void dsd_iq_replay_close(dsd_iq_replay_source* src);

/**
 * @brief Compute replayable bytes from metadata and on-disk size.
 */
int dsd_iq_replay_compute_effective_bytes(uint64_t data_bytes, uint64_t actual_file_size, dsd_iq_sample_format format,
                                          uint64_t* out_effective, int* out_size_mismatch);

/**
 * @brief The capture bytes a replay of @p cfg reads from a data file of @p file_size bytes.
 *
 * For a sidecar capture, dsd_iq_replay_compute_effective_bytes() of its data_bytes; @p out_mismatch is set when
 * data_bytes differs from the file. For a WAV, its whole frames in virtual cf32 bytes (8 per frame); @p out_mismatch
 * is set when the header declares more data than the file holds (a truncated recording).
 *
 * @param out_mismatch May be NULL.
 * @return DSD_IQ_OK or DSD_IQ_ERR_INVALID_ARG.
 */
int dsd_iq_replay_measure(const dsd_iq_replay_config* cfg, uint64_t file_size, uint64_t* out_effective,
                          int* out_mismatch);

/**
 * @brief Validate effective replay bytes for replay (not info-only paths).
 */
int dsd_iq_replay_validate_effective_bytes_for_replay(uint64_t effective_bytes, int loop);

/**
 * @brief Bytes the replay reader reads as one chunk, the replay's demod block.
 *
 * min(DSD_IQ_REPLAY_CHUNK_MAX_SAMPLES, sample_rate_hz x DSD_IQ_REPLAY_CHUNK_MAX_SAMPLES /
 * DSD_IQ_REPLAY_CHUNK_REFERENCE_RATE_HZ) complex samples, at least one, in the format's bytes. An event or the
 * capture's end can cut a chunk shorter.
 *
 * @return The chunk in bytes, or 0 for a sample rate of 0 or a format with no sample size.
 */
size_t dsd_iq_replay_chunk_bytes(dsd_iq_sample_format format, uint32_t sample_rate_hz);

/**
 * @brief Estimate capture duration from bytes and sample rate.
 *
 * @return Duration in seconds, or 0.0 when inputs are invalid.
 */
double dsd_iq_replay_estimate_duration_seconds(uint64_t data_bytes, dsd_iq_sample_format format,
                                               uint32_t sample_rate_hz);

/**
 * @brief Print stable human-readable metadata summary.
 */
int dsd_iq_info_print(const dsd_iq_replay_config* cfg, const char* display_path, uint64_t actual_file_size, FILE* out,
                      FILE* err);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_IO_IQ_REPLAY_H_ */
