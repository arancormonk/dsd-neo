// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/rtl_device_view.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/input_spec.h>
#include <stddef.h>

int
dsd_app_rtl_device_spec_field(const dsd_opts* opts, dsd_app_radio_row_field* out) {
    if (!opts || !out) {
        return -1;
    }
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        if (!dsd_opts_audio_in_dev_is_rtl_spec(opts->audio_in_dev)) {
            return -1;
        }
        *out = DSD_APP_RADIO_ROW_AUDIO_IN_DEV;
        return 0;
    }
    const dsd_app_radio_row_field field = dsd_app_radio_row_source_field(opts);
    if (field == DSD_APP_RADIO_ROW_DEFAULT) {
        *out = DSD_APP_RADIO_ROW_RADIO_IN_DEV;
        return 0;
    }
    if (!dsd_opts_audio_in_dev_is_rtl_spec(field == DSD_APP_RADIO_ROW_AUDIO_IN_DEV ? opts->audio_in_dev
                                                                                   : opts->radio_in_dev)) {
        return -1;
    }
    *out = field;
    return 0;
}

int
dsd_app_rtl_device_prefill(const dsd_opts* opts, char* out, size_t out_size) {
    if (!out || out_size == 0U) {
        return -1;
    }
    out[0] = '\0';
    dsd_app_radio_row_field field = DSD_APP_RADIO_ROW_DEFAULT;
    if (dsd_app_rtl_device_spec_field(opts, &field) != 0) {
        return -1;
    }
    const char* spec = field == DSD_APP_RADIO_ROW_AUDIO_IN_DEV ? opts->audio_in_dev : opts->radio_in_dev;
    if (!dsd_opts_audio_in_dev_is_rtl_spec(spec)) {
        spec = "rtl"; /* nothing remembered: the row opens a bare rtl */
    }
    int index = 0;
    char serial[DSD_RTL_SERIAL_MAX + 1];
    serial[0] = '\0';
    const dsd_rtl_dev_kind kind = dsd_rtl_spec_device(spec, &index, serial, sizeof serial);
    if (kind == DSD_RTL_DEV_INVALID) {
        return -1;
    }
    return dsd_rtl_device_field_format(index, kind == DSD_RTL_DEV_SERIAL ? serial : NULL, out, out_size);
}
