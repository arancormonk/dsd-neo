// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief IQ replay metadata and source reader API.
 */

#pragma once

#include <dsd-neo/io/iq_types.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
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
    int combine_rotate_enabled;
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
    int loop;
    int realtime;
} dsd_iq_replay_config;

typedef struct dsd_iq_replay_source dsd_iq_replay_source;

/**
 * @brief Parse replay metadata without opening a data stream.
 */
int dsd_iq_replay_read_metadata(const char* path, dsd_iq_replay_config* out_cfg, char* err_buf, size_t err_buf_size);

/**
 * @brief Parse replay metadata and optionally open the data stream.
 *
 * If @p out is NULL, metadata-only validation is performed.
 */
int dsd_iq_replay_open(const char* path, dsd_iq_replay_config* out_cfg, dsd_iq_replay_source** out, char* err_buf,
                       size_t err_buf_size);
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
