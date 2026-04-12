// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief IQ capture writer API.
 */

#pragma once

#include <dsd-neo/io/iq_types.h>
#include <stddef.h>
#include <stdint.h>

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
    char source_backend[32];
    char source_args[256];
    uint64_t max_bytes;
    size_t queue_block_bytes;
    size_t queue_block_count;
    void (*drop_warning_cb)(void* user, uint64_t dropped_bytes, uint64_t dropped_blocks);
    void* drop_warning_user;
} dsd_iq_capture_config;

typedef struct {
    uint64_t input_ring_drops;
    uint32_t retune_count;
} dsd_iq_capture_final_stats;

typedef struct dsd_iq_capture_writer dsd_iq_capture_writer;

/**
 * @brief Resolve data and metadata paths from a user path.
 *
 * If @p path ends with `.json`, it is treated as metadata path and the data
 * path is derived by stripping `.json`. Otherwise @p path is treated as the
 * data path and metadata becomes `<path>.json`.
 */
int dsd_iq_capture_derive_paths(const char* path, char* out_data_path, size_t out_data_path_size,
                                char* out_metadata_path, size_t out_metadata_path_size, char* err_buf,
                                size_t err_buf_size);

int dsd_iq_capture_open(const dsd_iq_capture_config* cfg, dsd_iq_capture_writer** out, char* err_buf,
                        size_t err_buf_size);
int dsd_iq_capture_submit(dsd_iq_capture_writer* writer, const void* data, size_t bytes);
void dsd_iq_capture_close(dsd_iq_capture_writer* writer, const dsd_iq_capture_final_stats* final_stats);
void dsd_iq_capture_abort(dsd_iq_capture_writer* writer);

#ifdef __cplusplus
}
#endif
