// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/runtime/cli.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/fp_mode.h>
#include <dsd-neo/runtime/log.h>

void
dsd_bootstrap_apply_fp_mode(void) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (!cfg) {
        dsd_neo_config_init();
        cfg = dsd_neo_get_config();
    }
    const dsd_fp_mode mode = dsd_runtime_fp_mode_from_config(cfg);
    dsd_runtime_fp_mode_publish(mode);
    dsd_runtime_fp_mode_apply_thread("decoder");
    if (mode == DSD_FP_MODE_INHERIT) {
        return;
    }
    if (!dsd_runtime_fp_mode_supported()) {
        LOG_INFO("NOTICE: DSD_NEO_FTZ_DAZ ignored: SSE FTZ/DAZ exists only on x86 builds\n");
        return;
    }
    LOG_INFO("NOTICE: SSE FTZ/DAZ %s for this run (env DSD_NEO_FTZ_DAZ)\n", mode == DSD_FP_MODE_ON ? "on" : "off");
}
