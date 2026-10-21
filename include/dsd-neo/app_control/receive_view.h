// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_APP_CONTROL_RECEIVE_VIEW_H
#define DSD_NEO_APP_CONTROL_RECEIVE_VIEW_H
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/scan_options.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/** Effective receive status shared by terminal, Qt and API frontends. */
void dsd_app_receive_status_text(const dsd_opts* opts, const dsd_state* state, char* out, size_t capacity);
/** Append "; never shown: DCC 7" to the edit notice @p notice (@p capacity bytes) when a scan row's options @p values
 * (NULL: the configured policies alone) over the configured receive and tone policies leave service names that only
 * rejected traffic could carry (dsd_scan_option_unreachable_names(), issue #637). The note lists whole identifiers;
 * the notice is shortened to keep it (dsd_receive_note_append()). Decoder thread. */
void dsd_app_receive_note_unreachable(const dsd_opts* opts, const dsd_state* state,
                                      const dsd_scan_option_values* values, char* notice, size_t capacity);
#ifdef __cplusplus
}
#endif
#endif
