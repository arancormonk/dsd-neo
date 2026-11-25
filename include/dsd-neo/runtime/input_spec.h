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
#include <dsd-neo/core/power.h>

#include <stddef.h>

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
 *   `soapy`/`soapy:<args>`. Each field is read in its place
 *   (split as dsd_spec_split_fields() splits, but a bracketed part such as
 *   SoapyRemote's `remote=[2001:db8::1]` keeps its colons in one field): an
 *   empty one leaves its option, and an empty args field is no args
 *   (`soapy::154.43M`).
 * - If trailing fields are ambiguous or not valid shorthand, the full string is
 *   treated as opaque Soapy args so valid third-party device strings are not
 *   misinterpreted as shorthand fields.
 * - Once the frequency place holds a frequency, the `sql` field is read as
 *   `--squelch` reads it, however many fields follow: one it refuses is logged
 *   naming the field and refuses the spec, leaving `opts` unchanged (issue
 *   #899 B14).
 * - A number with a K/M/G suffix in the frequency place (`5.8G`, `500k`) is
 *   always a frequency, never device args: one that does not parse or is
 *   outside 1 MHz to 4294.967295 MHz is logged as out of range and refused,
 *   leaving `opts` unchanged. A bare number out of range keeps the opaque
 *   fallback.
 *
 * @param opts Decoder options containing `audio_in_dev` and shared radio fields.
 * @param out_tuning_applied Optional out-flag set to 1 when shorthand tuning was
 *        parsed and applied; otherwise set to 0.
 * @return 0 on success; negative on invalid arguments, an out-of-range
 *         suffixed frequency or a refused squelch field.
 */
int dsd_normalize_soapy_input_spec(dsd_opts* opts, int* out_tuning_applied);

/**
 * @brief Read an input spec's `sql` field (`rtl:`, `rtltcp:`, `soapy:`, Airspy) as `--squelch` reads it
 * (dsd_squelch_setting_parse()), into @p out.
 *
 * A field the grammar refuses refuses the spec (issue #899 B14; it was once ignored, leaving the squelch as it was): it
 * is logged as an error naming the field, @p input (the spec's kind, "rtl:") and the grammar's reason.
 *
 * @return 0, or -1 with @p out untouched.
 */
int dsd_spec_squelch_field_parse(const char* input, const char* text, dsd_squelch_setting* out);

/**
 * @brief Read an RTL-SDR or rtl_tcp input spec into the options it sets.
 *
 * `rtl:dev:freq:gain:ppm:bw:sql:vol[:bias[=on|off]]` sets the device index, and
 * `rtltcp:host:port:freq:gain:ppm:bw:sql:vol[:bias[=on|off]]` the host and port, then the tuning fields in order:
 * the frequency (Hz, or with a k/M/G suffix), the tuner gain, the PPM, the DSP bandwidth (a selectable one,
 * dsd_analog_rtl_dsp_bw_is_selectable(); anything else reads as the 48 kHz default), the squelch (as `--squelch`
 * reads it, dsd_spec_squelch_field_parse()), the monitor volume and, after all of those, `bias` tokens. Each field is
 * read in its place (dsd_spec_split_fields()): an empty field, like one the spec stops before, leaves its option at what
 * it holds, so `rtl::851M` keeps the device. A tuning number that does not parse leaves its option; an rtl_tcp port
 * that is not a whole number from 1 to 65535, or a squelch field `--squelch` would refuse (issue #899 B14), is logged
 * and refuses the spec, which then changes nothing. The engine reads the spec this way when the input opens, and
 * --print-config before that, so both report the same settings.
 *
 * @return 1 when @p opts holds such a spec and it was read, 0 for any other input (nothing changed), -1 on bad
 *         arguments, a refused port or a refused squelch field.
 */
int dsd_rtl_input_spec_apply(dsd_opts* opts);

/**
 * @brief Read the host and port of a `kind[:host[:port]]` spec: `-i tcp:`, `-i udp:`, `-i m17udp:`, `-o udp:` and
 *        `-o m17udp:`.
 *
 * Each field is read in its place (dsd_spec_split_fields()), the kind's included but not checked here. An empty or
 * missing host or port field leaves @p host or @p port at what it holds, the caller's default: `udp::17355` keeps the
 * default bind address. A port field must be a whole decimal number from @p min_port to 65535, and a host must fit
 * @p host; anything else is logged with the spec and refuses it, with @p host and @p port unchanged. Fields after the
 * port are not read.
 *
 * @return 0 when read, -1 when refused or on bad arguments.
 */
int dsd_endpoint_spec_read(const char* spec, int min_port, char* host, size_t host_size, int* port);

/**
 * @brief The DSP bandwidth a spec's bandwidth field @p token names, in kHz: a selectable one
 * (dsd_analog_rtl_dsp_bw_is_selectable()), and the 48 kHz default for anything else (NULL included), as
 * dsd_rtl_input_spec_apply() reads it.
 */
int dsd_rtl_spec_bw_khz_or_default(const char* token);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_INPUT_SPEC_H_H */
