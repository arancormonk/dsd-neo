// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/frontend.h>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/app_control/history.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/frontend_types.h>
#include <dsd-neo/ui/native_provider.h>

#if defined(DSD_NEO_INCLUDE_DSD_NEO_CORE_OPTS_H_H)
#error "public frontend app-control headers must not include core/opts.h"
#endif

#if defined(DSD_NEO_INCLUDE_DSD_NEO_CORE_STATE_H_H)
#error "public frontend app-control headers must not include core/state.h"
#endif

#if defined(DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_CURSES_COMPAT_H_)
#error "public frontend app-control headers must not include terminal/curses headers"
#endif

#if defined(DSD_NEO_INCLUDE_DSD_NEO_UI_NCURSES_H_) || defined(DSD_NEO_INCLUDE_DSD_NEO_UI_UI_PRIMS_H_)
#error "public frontend app-control headers must not include terminal UI headers"
#endif

#if defined(DSD_NEO_INCLUDE_DSD_NEO_IO_RTL_STREAM_C_H_) || defined(DSD_NEO_INCLUDE_DSD_NEO_DSP_FRAME_SYNC_H_)
#error "public frontend app-control headers must not include IO/DSP headers"
#endif

#if defined(DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_H_)
#error "public frontend app-control headers must not include protocol headers"
#endif

int
main(void) {
    (void)sizeof(dsd_frontend_kind);
    (void)sizeof(dsd_frontend_status);
    (void)sizeof(dsd_frontend_metrics);
    (void)sizeof(dsd_frontend_snapshot);
    (void)sizeof(dsd_app_endpoint_payload);
    (void)sizeof(dsd_app_command_descriptor);
    (void)&dsd_app_command_descriptors_get;
    (void)&dsd_app_frontend_runtime_start;
    (void)&dsd_app_frontend_runtime_stop;
    (void)&dsd_app_frontend_history_get_mode;
    (void)&dsd_app_frontend_history_set_mode;
    (void)&dsd_app_frontend_history_cycle_mode;
    (void)&dsd_app_frontend_history_compact_event_text;
    (void)&dsd_app_frontend_history_event_sort_time;
    (void)&dsd_native_frontend_provider;
    return 0;
}
