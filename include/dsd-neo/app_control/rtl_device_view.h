// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The RTL-SDR spec the next local open reads: where an RTL-SDR device change writes its selector, and what the
 * frontends' Device prompts open on. The spec alone names the dongle (dsd_rtl_spec_select_device()).
 */
#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_RTL_DEVICE_VIEW_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_RTL_DEVICE_VIEW_H_

#include <dsd-neo/app_control/radio_source_view.h>
#include <dsd-neo/core/opts_fwd.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The field holding the spec the next local RTL-SDR open reads, which an RTL-SDR device change rewrites: with an
 * RTL-family input in force, dsd_opts::audio_in_dev, which must then be a local `rtl` spec; otherwise the RTL-SDR row's
 * (dsd_app_radio_row_source_field()), dsd_opts::radio_in_dev when that row would open a bare "rtl" (no RTL-SDR spec
 * remembered; the change creates one there). 0, or -1 when that open is no local RTL-SDR (another radio, or another
 * input staged in place of the running one), with @p out unchanged.
 */
int dsd_app_rtl_device_spec_field(const dsd_opts* opts, dsd_app_radio_row_field* out);

/**
 * @brief The RTL-SDR the spec dsd_app_rtl_device_spec_field() names, as the Device prompt shows it: its index, or
 * `serial=<sn>` (so a serial of digits stays a serial), "0" for a bare `rtl` or an empty field (index 0, as every
 * reader takes it). The spec alone names it, never the device the options last opened. 0, or -1 with @p out "" when
 * that open is no local RTL-SDR or its field is refused.
 */
int dsd_app_rtl_device_prefill(const dsd_opts* opts, char* out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_RTL_DEVICE_VIEW_H_ */
