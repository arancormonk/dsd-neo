// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_APP_CONTROL_RECEIVE_VIEW_H
#define DSD_NEO_APP_CONTROL_RECEIVE_VIEW_H
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/** Effective receive status shared by terminal, Qt and API frontends. */
void dsd_app_receive_status_text(const dsd_opts* opts, const dsd_state* state, char* out, size_t capacity);
#ifdef __cplusplus
}
#endif
#endif
