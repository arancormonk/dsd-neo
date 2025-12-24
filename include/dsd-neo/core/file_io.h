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

struct sf_private_tag* open_wav_file(char* dir, char* temp_filename, uint16_t sample_rate, uint8_t ext);
void openWavOutFileRaw(dsd_opts* opts, dsd_state* state);
void openSymbolOutFile(dsd_opts* opts, dsd_state* state);

#ifdef __cplusplus
}
#endif
