// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/scan_mode.h>

/* Label/picker tests inject preset labels; scoped metadata is covered by runtime and snapshot tests. */
dsdneoUserDecodeMode
dsd_scan_mode_configured_preset(const dsd_opts* opts, const dsd_state* state) {
    (void)state;
    return dsd_infer_decode_mode_preset(opts);
}

dsd_scan_mode
dsd_scan_mode_active(const dsd_state* state) {
    (void)state;
    return DSD_SCAN_MODE_INHERIT;
}

const char*
dsd_scan_mode_name(dsd_scan_mode mode) {
    (void)mode;
    return "";
}
