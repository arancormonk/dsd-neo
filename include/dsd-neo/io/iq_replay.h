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

#include <dsd-neo/core/iq_replay_checkpoint.h>
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
    /* 1 when the capture did not know its tuner gain ("tuner_gain_known": false; or, in metadata written before that
     * field existed, an Airspy capture, whose tuner never reported one): tuner_gain_tenth_db is then no reading. */
    int tuner_gain_unknown;
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
    /* The complex rate the demodulator takes. 0 means sample_rate_hz, the capture's own rate. A replay's rate plan
     * (dsd_iq_replay_apply_rate_plan()) sets it; base_decimation, post_downsample and demod_rate_hz then describe the
     * chain from it, not from sample_rate_hz. */
    uint32_t dsp_input_rate_hz;
    /* The DSP bandwidth the capture recorded (the sidecar's rtl_dsp_bw_khz), as read; 0 for a WAV. */
    int recorded_dsp_bw_khz;
    /* Something the open wants the user told (a sidecar beside a WAV that the WAV's header overrides), or empty. */
    char open_notice[DSD_IQ_REPLAY_NOTICE_SIZE];
    /* A run-time re-plan's resume (issue #701): when resume is set, the replay opens at resume_at
     * (dsd_iq_replay_resume_check()) instead of the capture's start. Set by the stream start, never read from a file. */
    int resume;
    dsd_iq_replay_checkpoint resume_at;
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

/* The rate plan's limits (issue #701): the slowest capture a replay resamples, the most it upsamples one, and how far
 * a replay's decimation may run past the rate the plan resamples to. */
#define DSD_IQ_REPLAY_MIN_CAPTURE_RATE_HZ 8000U
#define DSD_IQ_REPLAY_MAX_UPSAMPLE_RATIO  9U
/* The longest half-band chain the plan picks, 2^10 (DSD_IQ_REPLAY_MAX_BASE_DECIMATION). */
#define DSD_IQ_REPLAY_MAX_PLAN_PASSES     10U

/** The rate chain a replay runs (dsd_iq_replay_plan_rate()). */
typedef struct {
    uint32_t capture_rate_hz;   /**< Fs: the capture's own rate, which paces the replay */
    uint32_t dsp_input_rate_hz; /**< R: the complex rate the demodulator takes; Fs, or the rate Fs is resampled to */
    uint32_t base_decimation;   /**< R / demod (x post_downsample): the half-band chain's decimation, a power of two */
    uint32_t post_downsample;
    uint32_t demod_rate_hz;
    uint32_t target_dsp_hz; /**< D: the DSP rate the plan was asked for (0 when the recorded chain was kept) */
    int kept_recorded;      /**< the sidecar's recorded chain runs as it was captured */
    int resample;           /**< dsp_input_rate_hz differs from capture_rate_hz */
} dsd_iq_replay_rate_plan;

/**
 * @brief The complex rate the replay of @p cfg hands the demodulator: dsp_input_rate_hz, or the capture's rate when
 *        that is 0.
 */
uint32_t dsd_iq_replay_dsp_input_rate_hz(const dsd_iq_replay_config* cfg);

/**
 * @brief Plan the rate chain a replay of @p cfg runs to reach @p target_dsp_hz (issue #701).
 *
 * - KEEP: a sidecar capture with no explicit request (@p explicit_target 0) keeps the chain it recorded when the mode
 *   has no rate requirement (@p cqpsk_required_multiple_hz 0), or when its post_downsample is 1 and the rate the CQPSK
 *   path runs at (dsp input / base_decimation; CQPSK skips post_downsample) is a multiple of the requirement. A
 *   recorded chain that misses the requirement only through a power-of-two post_downsample folds it into
 *   base_decimation when that reaches a multiple.
 * - Otherwise (a WAV, an explicit request, or a recorded chain the mode cannot run), with D = @p target_dsp_hz: a
 *   capture at D x 2^k (k up to 10) decimates by 2^k with no resampling; any other is resampled to R = D x 2^k, the
 *   largest at or below the capture's rate (R = D when the capture is slower than D), and decimated by 2^k, with
 *   post_downsample 1 and the demodulator at D.
 *
 * Refused: a capture below DSD_IQ_REPLAY_MIN_CAPTURE_RATE_HZ, an upsample past DSD_IQ_REPLAY_MAX_UPSAMPLE_RATIO, a
 * capture 2 x 2^10 x D or faster (the resampler would decimate by 2 or more), and a target of 0.
 *
 * @param out_plan [out] The plan; set only on success.
 * @return DSD_IQ_OK, DSD_IQ_ERR_RATE_CHAIN with the reason in @p err_buf, or DSD_IQ_ERR_INVALID_ARG.
 */
int dsd_iq_replay_plan_rate(const dsd_iq_replay_config* cfg, uint32_t target_dsp_hz,
                            uint32_t cqpsk_required_multiple_hz, int explicit_target, dsd_iq_replay_rate_plan* out_plan,
                            char* err_buf, size_t err_buf_size);

/**
 * @brief Write @p plan's chain into @p cfg: dsp_input_rate_hz, base_decimation, post_downsample and demod_rate_hz.
 *
 * sample_rate_hz stays the capture's rate: pacing, media time, chunking and event offsets follow the capture.
 *
 * @return DSD_IQ_OK, or DSD_IQ_ERR_INVALID_ARG for a NULL argument or a plan made for another capture rate.
 */
int dsd_iq_replay_apply_rate_plan(dsd_iq_replay_config* cfg, const dsd_iq_replay_rate_plan* plan);

/**
 * @brief Capture bytes the replay reader reads as one chunk when the capture at @p capture_hz is resampled to
 *        @p dsp_in_hz.
 *
 * At equal rates, exactly dsd_iq_replay_chunk_bytes(). Otherwise min(DSD_IQ_REPLAY_CHUNK_MAX_SAMPLES, capture_hz x
 * DSD_IQ_REPLAY_CHUNK_MAX_SAMPLES / DSD_IQ_REPLAY_CHUNK_REFERENCE_RATE_HZ, floor((DSD_IQ_REPLAY_CHUNK_MAX_SAMPLES - 1)
 * x capture_hz / dsp_in_hz)) complex samples, at least one: the chunk spans no longer than a live transfer, and what
 * it resamples to still fits one (issue #626).
 *
 * @return The chunk in bytes, or 0 for a rate of 0 or a format with no sample size.
 */
size_t dsd_iq_replay_chunk_bytes_for(dsd_iq_sample_format format, uint32_t capture_hz, uint32_t dsp_in_hz);

/**
 * @brief Check that the replay of @p cfg can resume at @p ckpt, and give the centre in force there (issue #701).
 *
 * The checkpoint must name an event index no further than the event list, past every event before data_offset and
 * before every event after it (one at data_offset itself may be either side), and carry an fs/4 phase of 0 to 3; an
 * offset past the capture's bytes is refused when the reader seeks to it (dsd_iq_replay_seek()). The centre is the one
 * the last RETUNE before event_index moved the capture to, or the capture's opening centre when none did.
 *
 * @param out_center         [out] The channel centre in force at the checkpoint (may be NULL).
 * @param out_capture_center [out] The capture (tuner) centre in force there (may be NULL).
 * @return DSD_IQ_OK; DSD_IQ_ERR_INVALID_ARG with the reason in @p err_buf for a checkpoint this capture cannot resume.
 */
int dsd_iq_replay_resume_check(const dsd_iq_replay_config* cfg, const dsd_iq_replay_checkpoint* ckpt,
                               uint64_t* out_center, uint64_t* out_capture_center, char* err_buf, size_t err_buf_size);

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
