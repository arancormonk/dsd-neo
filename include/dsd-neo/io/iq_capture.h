// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief IQ capture writer API.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_IO_IQ_CAPTURE_H_
#define DSD_NEO_INCLUDE_DSD_NEO_IO_IQ_CAPTURE_H_

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
    /**
     * Age at which a partially filled queue block is written out even though it
     * is not full, in milliseconds. Bounds how long submitted bytes can sit in
     * memory when the stream goes quiet (muted channel, reconfigure hold), which
     * is the window lost if the process is killed. 0 selects the default.
     */
    uint32_t queue_flush_interval_ms;
    void (*drop_warning_cb)(void* user, uint64_t dropped_bytes, uint64_t dropped_blocks);
    void* drop_warning_user;
    /**
     * Called once, the first time @ref max_bytes stops a submission. Reaching the
     * limit is not data loss and deliberately does not raise drop_warning_cb, but
     * it does end the capture, so it needs its own signal.
     */
    void (*size_limit_cb)(void* user, uint64_t max_bytes);
    void* size_limit_user;
} dsd_iq_capture_config;

typedef struct {
    uint64_t input_ring_drops;
    uint32_t retune_count;
} dsd_iq_capture_final_stats;

typedef struct dsd_iq_capture_writer dsd_iq_capture_writer;

/**
 * @brief Resolve data and metadata paths from a user path.
 *
 * If @p path ends with `.json` (matched case-insensitively), it is treated as the
 * metadata path and the data path is derived by stripping `.json`. Otherwise @p path
 * is the data path and metadata becomes `<data path>.json`.
 *
 * A path whose final component carries no extension gains the conventional `.iq`, so
 * `mycap` yields `mycap.iq` plus `mycap.iq.json`. A dot in a directory name does not
 * count as an extension, and a leading dot belongs to the name (`.hidden` has none).
 * The suffix does not vary with the sample format; that is recorded in the sidecar.
 */
int dsd_iq_capture_derive_paths(const char* path, char* out_data_path, size_t out_data_path_size,
                                char* out_metadata_path, size_t out_metadata_path_size, char* err_buf,
                                size_t err_buf_size);

int dsd_iq_capture_open(const dsd_iq_capture_config* cfg, dsd_iq_capture_writer** out, char* err_buf,
                        size_t err_buf_size);
int dsd_iq_capture_submit(dsd_iq_capture_writer* writer, const void* data, size_t bytes);
int dsd_iq_capture_record_event(dsd_iq_capture_writer* writer, const dsd_iq_event* event);
void dsd_iq_capture_close(dsd_iq_capture_writer* writer, const dsd_iq_capture_final_stats* final_stats);
void dsd_iq_capture_abort(dsd_iq_capture_writer* writer);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_IO_IQ_CAPTURE_H_ */
