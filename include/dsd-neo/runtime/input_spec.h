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

/** How an `rtl:` spec's device field names its dongle (dsd_rtl_device_field_parse()). */
typedef enum {
    DSD_RTL_DEV_EMPTY = 0, /* no device field: the device stays as it is */
    DSD_RTL_DEV_INDEX,     /* a device index */
    DSD_RTL_DEV_SERIAL,    /* a USB serial */
    DSD_RTL_DEV_INVALID    /* neither: refused */
} dsd_rtl_dev_kind;

/**
 * @brief 1 when @p serial can name an RTL-SDR: 1 to DSD_RTL_SERIAL_MAX bytes of printable ASCII (0x20-0x7E), with no
 *        ':' (the spec's field separator) or '"' (the INI quote), and no space at either end; 0 otherwise.
 */
int dsd_rtl_serial_is_valid(const char* serial);

/**
 * @brief Read an `rtl:` spec's device field.
 *
 * A decimal number with no leading zero (`0`, `1`, `12`) that fits an int is a device index, so every script that
 * names its dongle by index reads as before. `serial=<sn>` is always a serial, for a serial such as `1090` that would
 * read as an index. Any other text (`00000001`, `0000001`, `A1B2`) is a serial. A serial must pass
 * dsd_rtl_serial_is_valid(). Nothing needs the hardware: the field means the same with or without dongles.
 *
 * @param field The field's text (NULL or "" is DSD_RTL_DEV_EMPTY).
 * @param index Receives the index for DSD_RTL_DEV_INDEX (may be NULL); untouched otherwise.
 * @param serial Receives the serial for DSD_RTL_DEV_SERIAL (may be NULL); untouched otherwise.
 * @param serial_size Capacity of @p serial; a serial that does not fit is DSD_RTL_DEV_INVALID.
 */
dsd_rtl_dev_kind dsd_rtl_device_field_parse(const char* field, int* index, char* serial, size_t serial_size);

/**
 * @brief Write the device field that names @p serial (when non-empty) or @p index: `serial=<sn>` (always the explicit
 *        form, so a serial never reads back as an index) or the index in decimal.
 *
 * @return 0, or -1 on bad arguments, an invalid serial, a negative index or a field that does not fit @p out (then "").
 */
int dsd_rtl_device_field_format(int index, const char* serial, char* out, size_t out_size);

/**
 * @brief Set the device field of the `rtl` / `rtl:...` spec in @p spec to @p serial (when non-empty) or @p index
 *        (dsd_rtl_device_field_format()), keeping every other field: `rtl` becomes `rtl:<dev>`.
 *
 * The new spec is built aside and copied only when it fits @p spec_size.
 *
 * @return 0 when rewritten; -1 when @p spec is not an `rtl` spec, on bad arguments, or when the result does not fit
 *         (then @p spec is unchanged).
 */
int dsd_rtl_spec_set_device(char* spec, size_t spec_size, int index, const char* serial);

/**
 * @brief Read the device field of the `rtl` / `rtl:...` spec @p spec (dsd_rtl_device_field_parse()): DSD_RTL_DEV_EMPTY
 *        for a bare `rtl` or an empty field (index 0, no serial, as every reader takes it), DSD_RTL_DEV_INVALID for a
 *        field the grammar refuses, one too long for any serial, or a spec that is no `rtl` one.
 */
dsd_rtl_dev_kind dsd_rtl_spec_device(const char* spec, int* index, char* serial, size_t serial_size);

/**
 * @brief Select in @p opts the dongle the device field of the `rtl` / `rtl:...` spec @p spec names, as a fresh start of
 *        that spec opens it: an index (the serial cleared), a serial (dsd_rtl_device_field_parse()), or, for a bare
 *        `rtl` or an empty field, index 0 with no serial. The spec is the one record of which dongle an input opens;
 *        whatever opens a receiver from a spec it did not just write (the RTL-SDR row, a config's spec) selects it here
 *        first, so the options never open another dongle than the spec a save writes.
 *
 * @return 1 when it selected, 0 when @p spec is no `rtl` spec (nothing changes), -1 for a device field the grammar
 *         refuses or bad arguments (nothing changes).
 */
int dsd_rtl_spec_select_device(dsd_opts* opts, const char* spec);

/**
 * @brief Lower-case the scheme of an input spec written in another case (`RTL:0:851M` → `rtl:0:851M`), in place.
 *
 * Only a scheme followed by ':' that matches a known input kind (pulse, rtl, rtltcp, airspy, soapy, iqreplay, iqtcp,
 * iqudp, tcp, udp, m17udp) ignoring case is changed; the rest of the spec, and any other input, is left as written.
 */
void dsd_input_spec_normalize_scheme(char* spec);

/**
 * @brief Read an RTL-SDR or rtl_tcp input spec into the options it sets.
 *
 * `rtl:dev:freq:gain:ppm:bw:sql:vol[:bias[=on|off]]` sets the device (an index or a serial,
 * dsd_rtl_device_field_parse(); one clears the other, and a field it refuses is logged and refuses the spec), and
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
 *         arguments, a refused device field, port or squelch field.
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
