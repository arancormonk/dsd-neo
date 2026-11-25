// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Frontend-neutral names for a radio input (issue #743, B5): what a terminal label or row says the receiver is.
 *
 * Every radio input once read "RTL-SDR dev N" on the terminal, rtl_tcp, Airspy and I/Q replay included. This view names
 * each kind by its own spec: "RTL-SDR dev 0", "rtl_tcp radio.example:1234", "Airspy", "SoapySDR [driver=lime]",
 * "I/Q replay capture.iq.json", "I/Q stream 127.0.0.1:1234 (cs16, 240 kHz)", and the device Input > Switch source >
 * RTL-SDR opens (dsd_app_radio_row_destination()).
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_RADIO_SOURCE_VIEW_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_RADIO_SOURCE_VIEW_H_

#include <dsd-neo/core/opts_fwd.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Room any name dsd_app_radio_source_name() writes takes with a host of any length cut to fit, terminator included. */
#define DSD_APP_RADIO_SOURCE_NAME_MAX 192

/**
 * @brief An I/Q stream spec (`iqtcp:` / `iqudp:`) as frontends show it: @p endpoint "HOST:PORT" for a TCP stream, "UDP
 * :PORT" for a UDP one bound to every address (0.0.0.0) and "UDP BIND:PORT" otherwise; @p detail its format and rate,
 * "cs16, 240 kHz".
 *
 * @return 0, or -1 (both outputs empty) when @p spec does not parse as a stream spec.
 */
int dsd_app_iq_stream_describe(const char* spec, char* endpoint, size_t endpoint_size, char* detail,
                               size_t detail_size);

/**
 * @brief The name of the radio input @p dev (a radio spec, dsd_opts_audio_in_dev_is_radio_spec()). The running input's
 * own readings (@p opts: device index, rtl_tcp server, Airspy serial) name it when @p dev is the input in force; a spec
 * that is not names itself from its text. A host too long for @p out_size is cut, so the format and rate after it
 * stay.
 */
void dsd_app_radio_source_name(const dsd_opts* opts, const char* dev, char* out, size_t out_size);

/** Which options field the radio row opens (dsd_app_radio_row_destination()). */
typedef enum {
    DSD_APP_RADIO_ROW_DEFAULT = 0, /* neither: the bare "rtl" */
    DSD_APP_RADIO_ROW_AUDIO_IN_DEV,
    DSD_APP_RADIO_ROW_RADIO_IN_DEV
} dsd_app_radio_row_field;

/**
 * @brief The field whose spec DSD_APP_CMD_RTL_ENABLE_INPUT opens (issue #634), the first of: dsd_opts::audio_in_dev when
 * it is a radio spec other than an Airspy one (the running radio, or one a config staged while PCM runs); the radio
 * spec a switch to PCM replaced (dsd_opts::radio_in_dev) unless it is an Airspy one; neither. Whatever rewrites the
 * spec that switch opens (an RTL-SDR device change) picks it with this rule.
 */
dsd_app_radio_row_field dsd_app_radio_row_source_field(const dsd_opts* opts);

/**
 * @brief The device DSD_APP_CMD_RTL_ENABLE_INPUT opens: the spec of dsd_app_radio_row_source_field(), or "rtl".
 */
void dsd_app_radio_row_destination(const dsd_opts* opts, char* out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_RADIO_SOURCE_VIEW_H_ */
