// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/engine/trunk_tuning.h>
#ifdef USE_RADIO
#include <dsd-neo/io/rtl_stream_c.h>
#endif
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stddef.h>
#include <stdint.h>

#include "engine_hooks_install.h"

#ifdef USE_RADIO
static void
dsd_engine_rtl_tune_completion(uint64_t request_id, rtl_stream_tune_result result, void* user_data) {
    (void)user_data;
    dsd_trunk_tune_result tune_result = DSD_TRUNK_TUNE_RESULT_FAILED;
    switch (result) {
        case RTL_STREAM_TUNE_OK: tune_result = DSD_TRUNK_TUNE_RESULT_OK; break;
        case RTL_STREAM_TUNE_DEFERRED: tune_result = DSD_TRUNK_TUNE_RESULT_DEFERRED; break;
        case RTL_STREAM_TUNE_TIMEOUT: tune_result = DSD_TRUNK_TUNE_RESULT_TIMEOUT; break;
        case RTL_STREAM_TUNE_FAILED:
        default: tune_result = DSD_TRUNK_TUNE_RESULT_FAILED; break;
    }
    dsd_trunk_tuning_request_publish(request_id, tune_result);
}
#endif

void
dsd_engine_trunk_tuning_hooks_install(void) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_freq_result = dsd_engine_trunk_tune_to_freq;
    hooks.tune_to_cc_result = dsd_engine_trunk_tune_to_cc;
    hooks.return_to_cc_result = dsd_engine_return_to_cc;
    hooks.tune_to_freq_request = dsd_engine_trunk_tune_to_freq_request;
    hooks.tune_to_cc_request = dsd_engine_trunk_tune_to_cc_request;
    hooks.return_to_cc_request = dsd_engine_return_to_cc_request;
    dsd_trunk_tuning_hooks_set(hooks);
#ifdef USE_RADIO
    rtl_stream_register_tune_completion_callback(dsd_engine_rtl_tune_completion, NULL);
#endif
}
