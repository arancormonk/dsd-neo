// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Input-spec normalization helpers for runtime/engine startup.
 *
 * Provides a SoapySDR-specific parser that supports an RTL-like shorthand:
 * `soapy[:args]:freq[:gain[:ppm[:bw[:sql[:vol]]]]]`.
 *
 * On successful shorthand parsing, the function updates shared radio tuning
 * fields in `dsd_opts` and normalizes `audio_in_dev` to `soapy` or
 * `soapy:<args>` so only device args are passed to Soapy.
 */

#pragma once

#include <dsd-neo/core/opts_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Normalize/parses Soapy input shorthand in `opts->audio_in_dev`.
 *
 * Behavior:
 * - Non-Soapy inputs are ignored (no-op, success).
 * - `soapy` and `soapy:<args>` are preserved.
 * - `soapy[:args]:freq[:gain[:ppm[:bw[:sql[:vol]]]]]` applies parsed tuning to
 *   existing `rtl_*` option fields and normalizes `audio_in_dev` to
 *   `soapy`/`soapy:<args>`.
 * - If trailing fields are ambiguous or not valid shorthand, the full string is
 *   treated as opaque Soapy args for backward compatibility.
 *
 * @param opts Decoder options containing `audio_in_dev` and shared radio fields.
 * @param out_tuning_applied Optional out-flag set to 1 when shorthand tuning was
 *        parsed and applied; otherwise set to 0.
 * @return 0 on success; negative on invalid arguments.
 */
int dsd_normalize_soapy_input_spec(dsd_opts* opts, int* out_tuning_applied);

#ifdef __cplusplus
}
#endif
