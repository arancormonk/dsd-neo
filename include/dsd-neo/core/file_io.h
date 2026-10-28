// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Core file I/O helpers used by CLI/UI orchestration.
 *
 * Declares file-related helpers implemented in core.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_FILE_IO_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_FILE_IO_H_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/sndfile_fwd.h>

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

void saveImbe4400Data(dsd_opts* opts, const dsd_state* state, const char* imbe_d);
/* AMBE 2450 records (.amb): the error byte packs the frame's C0 and total error counts behind DSD_MBE_AMB_ERR_FLAG,
   which -r replay restores as C0 context and error rate. A byte without the flag is a total alone (older captures,
   other tools). */
#define DSD_MBE_AMB_ERR_FLAG      0x80U
#define DSD_MBE_AMB_ERR_C0_SHIFT  4
#define DSD_MBE_AMB_ERR_C0_MAX    7
#define DSD_MBE_AMB_ERR_TOTAL_MAX 15
void saveAmbe2450Data(dsd_opts* opts, const dsd_state* state, const char* ambe_d);
void saveAmbe2450DataR(dsd_opts* opts, const dsd_state* state, const char* ambe_d);
int dsd_frame_log_enabled(const dsd_opts* opts);
int dsd_frame_detail_enabled(const dsd_opts* opts);
void dsd_frame_logf(dsd_opts* opts, const char* format, ...) DSD_ATTR_FORMAT(printf, 2, 3);
void dsd_frame_log_close(dsd_opts* opts);
int dsd_p25_sm_log_enabled(const dsd_opts* opts);
void dsd_p25_sm_logf(dsd_opts* opts, const char* format, ...) DSD_ATTR_FORMAT(printf, 2, 3);
void dsd_p25_sm_log_close(dsd_opts* opts);
void PrintAMBEData(dsd_opts* opts, const dsd_state* state, const char* ambe_d);
void PrintIMBEData(dsd_opts* opts, const dsd_state* state, const char* imbe_d);
int readImbe4400Data(dsd_opts* opts, dsd_state* state, char* imbe_d);
int readAmbe2450Data(dsd_opts* opts, dsd_state* state, char* ambe_d);
/** Replay an SDRTrunk JSON MBE file from the start of opts->mbe_in_f.
 * Supports P25, DMR, and NXDN enhanced-half-rate voice. Uses supplied keys;
 * encrypted NXDN DES/AES frames without an IV and DMR AES frames without
 * explicit or verified late-entry context remain muted. Caller initializes
 * FEC tables and vocoder state as for ordinary MBE playback.
 */
void read_sdrtrunk_json_format(dsd_opts* opts, dsd_state* state);
void openMbeInFile(dsd_opts* opts, dsd_state* state);

/** Record format of an MBE capture (-d) file; its 4-byte header and its extension name it. */
typedef enum {
    DSD_MBE_OUT_FORMAT_NONE = 0,
    DSD_MBE_OUT_FORMAT_IMBE = 1,  /**< .imb: IMBE 4400 records (P25 Phase 1, ProVoice, YSF full rate) */
    DSD_MBE_OUT_FORMAT_AMBE = 2,  /**< .amb: AMBE 2450 records */
    DSD_MBE_OUT_FORMAT_DSTAR = 3, /**< .dmb: D-STAR AMBE 2400 frames in AMBE records */
} dsd_mbe_out_format;

/** Slot masks for dsd_mbe_out_close_protocol(): the slot 1 and slot 2 capture files. */
#define DSD_MBE_OUT_SLOT1 1
#define DSD_MBE_OUT_SLOT2 2
#define DSD_MBE_OUT_BOTH  3

/**
 * MBE capture (-d), issue #661. A capture file holds one owner's frames in one record format: the capture writer
 * creates it in opts->mbe_out_dir with the first frame written to it, and closes it for a frame of another owner or
 * format (on either slot) before it creates the next one. A handle a caller attached itself (owner 0) is written as it
 * is. The owner groups the synctypes of one protocol: P25 Phase 1 and 2 share one (their formats keep them apart) and
 * EDACS shares one with ProVoice.
 */
int dsd_mbe_out_owner_of(int synctype);
/** The protocol a frame belongs to: the synctype processFrame() is dispatching, else state->synctype. */
int dsd_mbe_out_frame_protocol(const dsd_state* state);
/** Whether slot (0 or 1) captures at all: -d is set, or a handle is open. Callers test it before any capture gate. */
int dsd_mbe_out_enabled(const dsd_opts* opts, int slot);
/**
 * Write one frame (bits as the vocoder holds them: 88 IMBE or 49 AMBE) to slot's capture, creating it if need be.
 * Refuses a frame read across a carrier boundary during its dispatch, and a YSF frame before a FICH has verified the
 * transmission on this carrier. The caller has already applied the frame's own output and record gates.
 */
void dsd_mbe_out_write(dsd_opts* opts, dsd_state* state, int slot, dsd_mbe_out_format format, const char* bits);
/** dsd_mbe_out_write() into file slot @p slot of a frame received on @p rx_slot, which the receive policy judges
 * (issue #637): X2-TDMA records both of its slots in the one slot-1 file. */
void dsd_mbe_out_write_rx(dsd_opts* opts, dsd_state* state, int slot, int rx_slot, dsd_mbe_out_format format,
                          const char* bits);
/** Close the captures in slot_mask that belong to synctype's owner, or that a caller attached (owner 0). */
void dsd_mbe_out_close_protocol(dsd_opts* opts, dsd_state* state, int slot_mask, int synctype);
void openWavOutFileLR(dsd_opts* opts, dsd_state* state);
SNDFILE* open_wav_file(char* dir, char* temp_filename, size_t temp_filename_size, uint16_t sample_rate, uint8_t ext);
void openWavOutFileRaw(dsd_opts* opts, dsd_state* state);
void openSymbolOutFile(dsd_opts* opts, dsd_state* state);
SNDFILE* close_wav_file(SNDFILE* wav_file);
/**
 * Write @p count samples to @p file (issue #708). On a per-call segment (one open_wav_file() opened) the first short
 * write latches the segment failed: nothing more is written to it, the failure is counted
 * (<dsd-neo/core/wav_recording.h>) and logged once per run, and the segment is closed and published as usual. Any other
 * handle (static or raw WAV) logs each short write and goes on, as before.
 */
void dsd_wav_segment_write(SNDFILE* file, const short* samples, size_t count, const char* context);
/** Test seam: replace the sf_write_short() and sf_close() the per-call recordings make; NULL restores libsndfile's. */
typedef long long (*dsd_wav_test_write_fn)(SNDFILE* file, const short* samples, long long count);
typedef int (*dsd_wav_test_close_fn)(SNDFILE* file);
void dsd_wav_test_set_io(dsd_wav_test_write_fn write_fn, dsd_wav_test_close_fn close_fn);
/** Test seam: let @p successful_opens more per-call segments open, then fail every open_wav_file(); -1 disables. */
void dsd_wav_test_fail_opens_after(long successful_opens);
/**
 * Close a per-call WAV, name it from the slot's staged row and export it to rdio-scanner.
 * `opened` is the decode time the file was opened (opts->wav_out_open_time or wav_out_open_timeR),
 * 0 when unknown; see dsd_rdio_export_call().
 */
SNDFILE* close_and_rename_wav_file(SNDFILE* wav_file, const dsd_opts* opts, const char* wav_out_filename,
                                   const char* dir, const Event_History_I* event_struct, time_t opened);
/**
 * As close_and_rename_wav_file(), but with the rdio-scanner export gated by `export_call`.
 * The event layer's drop path rotates the recording of an epoch that gets no history row or log
 * line; exporting it would upload audio tagged with all-zero metadata that the operator has
 * nothing local to correlate against.
 */
SNDFILE* close_and_rename_wav_file_ex(SNDFILE* wav_file, const dsd_opts* opts, const char* wav_out_filename,
                                      const char* dir, const Event_History_I* event_struct, time_t opened,
                                      int export_call);
/**
 * Close a conventional NFM call's recording that no longer has its history row (a teardown that skipped the call's
 * own end) and publish it under the NFM name, without a service name or an rdio export.
 */
SNDFILE* close_and_rename_analog_wav_file(SNDFILE* wav_file, const char* wav_out_filename, const char* dir);
void closeMbeOutFile(dsd_opts* opts, dsd_state* state);
void closeMbeOutFileR(dsd_opts* opts, dsd_state* state);
void closeSymbolOutFile(dsd_opts* opts, dsd_state* state);
void rotate_symbol_out_file(dsd_opts* opts, dsd_state* state);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_FILE_IO_H_H */
