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

#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_INPUT_SPEC_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_INPUT_SPEC_H_H

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
 *   treated as opaque Soapy args so valid third-party device strings are not
 *   misinterpreted as shorthand fields.
 *
 * @param opts Decoder options containing `audio_in_dev` and shared radio fields.
 * @param out_tuning_applied Optional out-flag set to 1 when shorthand tuning was
 *        parsed and applied; otherwise set to 0.
 * @return 0 on success; negative on invalid arguments.
 */
int dsd_normalize_soapy_input_spec(dsd_opts* opts, int* out_tuning_applied);

/**
 * @brief Read an RTL-SDR or rtl_tcp input spec into the options it sets.
 *
 * `rtl:dev:freq:gain:ppm:bw:sql:vol[:bias[=on|off]]` sets the device index, and
 * `rtltcp:host:port:freq:gain:ppm:bw:sql:vol[:bias[=on|off]]` the host and port, then the tuning fields in order:
 * the frequency (Hz, or with a k/M/G suffix), the tuner gain, the PPM, the DSP bandwidth (4, 6, 8, 12, 16, 24 or 48
 * kHz; anything else reads as 48), the squelch (dsd_squelch_level_from_sql(); a field that is not a number leaves it),
 * the monitor volume and, after all of those, `bias` tokens. A spec that stops early sets what it has; a number that
 * does not parse leaves its option. The engine reads the spec this way when the input opens, and --print-config before
 * that, so both report the same settings.
 *
 * @return 1 when @p opts holds such a spec and it was read, 0 for any other input (nothing changed), -1 on bad
 *         arguments.
 */
int dsd_rtl_input_spec_apply(dsd_opts* opts);

/**
 * @brief The DSP bandwidth a spec's bandwidth field @p token names, in kHz: 4, 6, 8, 12, 16, 24 or 48, and 48 for
 * anything else (NULL included), as dsd_rtl_input_spec_apply() reads it.
 */
int dsd_rtl_spec_bw_khz_or_default(const char* token);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_INPUT_SPEC_H_H */
