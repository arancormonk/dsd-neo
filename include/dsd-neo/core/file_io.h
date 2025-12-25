// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Core file I/O helpers used by CLI/UI orchestration.
 *
 * Declares file-related helpers implemented in core so higher-level modules
 * can call them without including the `dsd.h` umbrella.
 */

#pragma once

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque sndfile handle (matches libsndfile's `SNDFILE` underlying struct tag). */
struct sf_private_tag;

void saveImbe4400Data(dsd_opts* opts, dsd_state* state, char* imbe_d);
void saveAmbe2450Data(dsd_opts* opts, dsd_state* state, char* ambe_d);
void saveAmbe2450DataR(dsd_opts* opts, dsd_state* state, char* ambe_d);
void PrintAMBEData(dsd_opts* opts, dsd_state* state, char* ambe_d);
void PrintIMBEData(dsd_opts* opts, dsd_state* state, char* imbe_d);
int readImbe4400Data(dsd_opts* opts, dsd_state* state, char* imbe_d);
int readAmbe2450Data(dsd_opts* opts, dsd_state* state, char* ambe_d);
void read_sdrtrunk_json_format(dsd_opts* opts, dsd_state* state);
void openMbeInFile(dsd_opts* opts, dsd_state* state);
void openMbeOutFile(dsd_opts* opts, dsd_state* state);
void openMbeOutFileR(dsd_opts* opts, dsd_state* state);
void openWavOutFile(dsd_opts* opts, dsd_state* state);
void openWavOutFileL(dsd_opts* opts, dsd_state* state);
void openWavOutFileR(dsd_opts* opts, dsd_state* state);
void openWavOutFileLR(dsd_opts* opts, dsd_state* state);
struct sf_private_tag* open_wav_file(char* dir, char* temp_filename, uint16_t sample_rate, uint8_t ext);
void openWavOutFileRaw(dsd_opts* opts, dsd_state* state);
void openSymbolOutFile(dsd_opts* opts, dsd_state* state);
struct sf_private_tag* close_wav_file(struct sf_private_tag* wav_file);
struct sf_private_tag* close_and_rename_wav_file(struct sf_private_tag* wav_file, char* wav_out_filename, char* dir,
                                                 Event_History_I* event_struct);
struct sf_private_tag* close_and_delete_wav_file(struct sf_private_tag* wav_file, char* wav_out_filename);
void closeMbeOutFile(dsd_opts* opts, dsd_state* state);
void closeMbeOutFileR(dsd_opts* opts, dsd_state* state);
void closeSymbolOutFile(dsd_opts* opts, dsd_state* state);
void rotate_symbol_out_file(dsd_opts* opts, dsd_state* state);
void closeWavOutFile(dsd_opts* opts, dsd_state* state);
void closeWavOutFileL(dsd_opts* opts, dsd_state* state);
void closeWavOutFileR(dsd_opts* opts, dsd_state* state);
void closeWavOutFileRaw(dsd_opts* opts, dsd_state* state);

#ifdef __cplusplus
}
#endif
