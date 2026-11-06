// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/analog_width_view.h>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/config_files.h>
#include <dsd-neo/app_control/frontend.h>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/app_control/history.h>
#include <dsd-neo/app_control/p25_metrics.h>
#include <dsd-neo/app_control/p25_network.h>
#include <dsd-neo/app_control/rx_tone_view.h>
#include <dsd-neo/app_control/scan_avoid_view.h>
#include <dsd-neo/app_control/scan_list_view.h>
#include <dsd-neo/app_control/scan_timing_view.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/app_control/squelch_view.h>
#include <dsd-neo/app_control/trunk_scan_validate.h>
#include <dsd-neo/core/frontend_types.h>

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
    (void)sizeof(dsd_app_p25_neighbor);
    (void)&dsd_app_p25_neighbors;
    (void)sizeof(dsd_app_p25_quality);
    (void)&dsd_app_p25_quality_from_state;
    (void)sizeof(dsd_frontend_kind);
    (void)sizeof(dsd_frontend_metrics);
    (void)sizeof(dsd_app_endpoint_payload);
    (void)&dsd_app_frontend_runtime_start;
    (void)&dsd_app_frontend_runtime_stop;
    (void)&dsd_app_frontend_redraw_consume;
    (void)&dsd_app_request_redraw;
    (void)&dsd_app_frontend_history_get_mode;
    (void)&dsd_app_frontend_history_set_mode;
    (void)&dsd_app_frontend_history_cycle_mode;
    (void)&dsd_app_frontend_history_compact_event_text;
    (void)&dsd_app_frontend_history_event_sort_time;
    (void)&dsd_app_get_latest_snapshot;
    (void)&dsd_app_get_latest_opts_snapshot;
    (void)&dsd_app_frontend_get_metrics_for_snapshot;
    (void)&dsd_app_trunk_scan_validate_targets_csv;
    (void)&dsd_app_rx_tone_view;
    (void)&dsd_app_scan_timing_view;
    (void)sizeof(dsd_app_scan_avoid_summary);
    (void)sizeof(dsd_app_scan_avoid_list);
    (void)sizeof(dsd_app_scan_avoid_raw);
    (void)sizeof(dsd_app_scan_avoid_remove_payload);
    (void)&dsd_app_scan_avoid_summary_get;
    (void)&dsd_app_scan_avoid_summary_same_key;
    (void)&dsd_app_scan_avoid_list_build;
    (void)&dsd_app_scan_avoid_list_free;
    (void)&dsd_app_scan_avoid_raw_collect;
    (void)&dsd_app_scan_avoid_raw_free;
    (void)&dsd_app_scan_avoid_raw_label;
    (void)&dsd_app_config_load_submit;
    (void)&dsd_app_config_load_profile_submit;
    (void)&dsd_app_config_list_profiles;
    (void)&dsd_app_config_save;
    (void)&dsd_app_config_default_missing;
    (void)&dsd_app_command_scan_avoid_remove;
    (void)sizeof(dsd_app_scan_list_view);
    (void)sizeof(dsd_app_scan_list_entry);
    (void)sizeof(dsd_app_scan_list_set_payload);
    (void)sizeof(dsd_app_scan_list_result);
    (void)&dsd_app_scan_list_view_get;
    (void)sizeof(dsd_app_scan_entry_summary);
    (void)&dsd_app_scan_entry_summary_get;
    (void)&dsd_app_scan_entry_eligible;
    (void)&dsd_app_command_scan_list_set;
    (void)&dsd_app_scan_list_request_next;
    (void)&dsd_app_scan_list_result_get;
    (void)&dsd_app_scan_list_results_since;
    (void)&dsd_app_scan_list_session_results_since;
    (void)sizeof(dsd_app_squelch_view);
    (void)&dsd_app_squelch_view_get;
    (void)&dsd_app_squelch_view_format;
    (void)sizeof(dsd_app_analog_width_view);
    (void)&dsd_app_analog_width_view_get;
    (void)&dsd_app_analog_width_view_format;
    (void)&dsd_app_analog_width_setting_format;
    (void)&dsd_app_analog_width_setting_text;
    (void)&dsd_app_analog_width_unset_hz;
    (void)&dsd_app_analog_rtl_bw_rate_hz;
    return 0;
}
