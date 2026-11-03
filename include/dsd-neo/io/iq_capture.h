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
    /** 1 when the tuner did not report its gain (an Airspy, a Soapy device without a gain range): written as
        "tuner_gain_known": false, and tuner_gain_tenth_db is then 0, not a reading. */
    int tuner_gain_unknown;
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

/** The sidecar's capture_end_reason of a capture that ended at a retune changing the device's sample rate (issue
 *  #806 X9). A capture that ran to its stream's end has no capture_end_reason. */
#define DSD_IQ_CAPTURE_END_RATE_CHANGED "rate_changed"

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
/**
 * @brief dsd_iq_capture_open(), also reporting whether it opened the data file for writing.
 *
 * Opening the data file for writing empties it, ending whatever an earlier capture recorded there, as soon as its
 * descriptor opens (dsd_fopen_private_ex()). An open that fails before that (invalid arguments or configuration, an
 * allocation, paths it cannot resolve, a data file it cannot open) leaves the data path as it found it. One that fails
 * after it (the stream over that descriptor, the metadata sidecar, the writer's queue or thread) removes the data file
 * it emptied.
 *
 * @param out_data_opened Set to 1 once the data file's descriptor was opened for writing, whether or not the open then
 *                        succeeds, and 0 otherwise. May be NULL.
 * @return As dsd_iq_capture_open().
 */
int dsd_iq_capture_open_ex(const dsd_iq_capture_config* cfg, dsd_iq_capture_writer** out, int* out_data_opened,
                           char* err_buf, size_t err_buf_size);
/**
 * @brief Submit captured bytes.
 *
 * @return DSD_IQ_OK, DSD_IQ_ERR_SEALED once the writer is sealed (nothing is taken), or another error.
 */
int dsd_iq_capture_submit(dsd_iq_capture_writer* writer, const void* data, size_t bytes);
/**
 * @brief Record an event at the current end of the capture.
 *
 * A RETUNE or RESET at another sample rate than the capture's cannot be replayed from this file (issue #806 X9): the
 * writer refuses it with DSD_IQ_ERR_RATE_CHAIN and is sealed. The file then ends with the samples before that event:
 * queued and partly packed blocks are still written, and every later submission and event is a no-op returning
 * DSD_IQ_ERR_SEALED. The writer stays open, and safe for producers still holding it, until dsd_iq_capture_close() at
 * the stream's end, which writes capture_end_reason DSD_IQ_CAPTURE_END_RATE_CHANGED in the sidecar.
 *
 * @return DSD_IQ_OK; DSD_IQ_ERR_RATE_CHAIN when this event sealed the writer; DSD_IQ_ERR_SEALED when it was sealed
 *         before; another error for an event refused on its own (the writer goes on).
 */
int dsd_iq_capture_record_event(dsd_iq_capture_writer* writer, const dsd_iq_event* event);
/**
 * @brief Fix the stream counts a sealed writer's sidecar reports.
 *
 * Called once the writer is sealed, with the caller's counts at the seal: dsd_iq_capture_close() reports them in
 * place of the final_stats it is given, which count retunes and drops after the file ended. Only the first call
 * counts.
 *
 * @return DSD_IQ_OK when the counts were taken, DSD_IQ_ERR_INVALID_ARG otherwise (not sealed, or taken before).
 */
int dsd_iq_capture_freeze_final_stats(dsd_iq_capture_writer* writer, const dsd_iq_capture_final_stats* stats);
void dsd_iq_capture_close(dsd_iq_capture_writer* writer, const dsd_iq_capture_final_stats* final_stats);
void dsd_iq_capture_abort(dsd_iq_capture_writer* writer);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_IO_IQ_CAPTURE_H_ */
