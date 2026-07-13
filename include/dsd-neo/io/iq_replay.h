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
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

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
    char source_backend[32];
    char source_args[256];
    char capture_started_utc[64];
    char notes[256];
    uint32_t event_count;
    dsd_iq_event* events;
    int loop;
    int realtime;
} dsd_iq_replay_config;

typedef struct dsd_iq_replay_source dsd_iq_replay_source;

/**
 * @brief Parse replay metadata without opening a data stream.
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
 * @brief Release owned replay config allocations and reset the struct to zero.
 *
 * Only call this on a zero-initialized config or a config returned by a
 * successful replay metadata/open call.
 */
void dsd_iq_replay_config_clear(dsd_iq_replay_config* cfg);
int dsd_iq_replay_read(dsd_iq_replay_source* src, void* out, size_t max_bytes, size_t* out_bytes);
int dsd_iq_replay_rewind(dsd_iq_replay_source* src);
void dsd_iq_replay_close(dsd_iq_replay_source* src);

/**
 * @brief Compute replayable bytes from metadata and on-disk size.
 */
int dsd_iq_replay_compute_effective_bytes(uint64_t data_bytes, uint64_t actual_file_size, dsd_iq_sample_format format,
                                          uint64_t* out_effective, int* out_size_mismatch);

/**
 * @brief Validate effective replay bytes for replay (not info-only paths).
 */
int dsd_iq_replay_validate_effective_bytes_for_replay(uint64_t effective_bytes, int loop);

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
