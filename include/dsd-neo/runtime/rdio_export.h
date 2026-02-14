// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief rdio-scanner export helpers (DirWatch sidecar + optional API upload).
 */

#pragma once

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DSD_RDIO_MODE_OFF = 0,
    DSD_RDIO_MODE_DIRWATCH = 1,
    DSD_RDIO_MODE_API = 2,
    DSD_RDIO_MODE_BOTH = 3,
};

/**
 * Parse textual rdio mode: off|dirwatch|api|both.
 *
 * @param s Input mode string.
 * @param mode_out Parsed mode output.
 * @return 0 on success, -1 on invalid input.
 */
int dsd_rdio_mode_from_string(const char* s, int* mode_out);

/**
 * Convert rdio mode value to text.
 *
 * @param mode Mode value.
 * @return String name ("off" for unknown values).
 */
const char* dsd_rdio_mode_to_string(int mode);

/**
 * Export a completed per-call WAV for rdio-scanner integration.
 *
 * Uses trunk-recorder compatible JSON metadata sidecars for DirWatch, and
 * optionally performs API upload when built with libcurl.
 *
 * @param opts Decoder options with rdio settings.
 * @param event_struct Event metadata for this call.
 * @param wav_path Final renamed WAV file path.
 * @return 0 when work completed (or mode disabled), -1 on export failure.
 */
int dsd_rdio_export_call(const dsd_opts* opts, const Event_History_I* event_struct, const char* wav_path);

#ifdef __cplusplus
}
#endif
