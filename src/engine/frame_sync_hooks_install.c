// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/edacs/edacs.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/config.h>

#ifdef USE_RTLSDR
#include <dsd-neo/io/rtl_stream_c.h>
#endif

#ifdef USE_RTLSDR
static void
dsd_engine_frame_sync_rf_mod_changed(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return;
    }
    if (opts->audio_in_type != AUDIO_IN_RTL || !state->rtl_ctx) {
        return;
    }

    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (!cfg) {
        dsd_neo_config_init(opts);
        cfg = dsd_neo_get_config();
    }
    /* Honor user override: do not fight DSD_NEO_CQPSK when set. */
    if (cfg && cfg->cqpsk_is_set) {
        return;
    }

    rtl_stream_toggle_cqpsk(state->rf_mod == 1 ? 1 : 0);
}
#endif

void
dsd_engine_frame_sync_hooks_install(void) {
    dsd_frame_sync_hooks hooks = {0};
    hooks.p25_sm_try_tick = p25_sm_try_tick;
    hooks.p25_sm_on_release = p25_sm_on_release;
    hooks.eot_cc = eot_cc;
    hooks.no_carrier = noCarrier;
#ifdef USE_RTLSDR
    hooks.rf_mod_changed = dsd_engine_frame_sync_rf_mod_changed;
#endif
    dsd_frame_sync_hooks_set(hooks);
}
