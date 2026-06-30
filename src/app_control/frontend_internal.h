// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_APP_CONTROL_FRONTEND_INTERNAL_H_
#define DSD_NEO_SRC_APP_CONTROL_FRONTEND_INTERNAL_H_

#include <dsd-neo/app_control/frontend.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

void dsd_app_frontend_status_from_opts_state(const dsd_opts* opts, const dsd_state* state, dsd_frontend_status* out);
int dsd_app_frontend_get_metrics_from_opts_state(const dsd_opts* opts, const dsd_state* state,
                                                 dsd_frontend_metrics* out);
int dsd_app_frontend_get_metrics_from_opts_state_with_snr_fallbacks(const dsd_opts* opts, const dsd_state* state,
                                                                    dsd_frontend_metrics* out,
                                                                    unsigned int snr_fallbacks);
int dsd_app_frontend_requested_ppm_from_opts(const dsd_opts* opts);
int dsd_app_frontend_auto_ppm_enabled_from_state(const dsd_state* state, int configured);
int dsd_app_frontend_tuner_autogain_enabled_from_state(const dsd_state* state, int configured);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_APP_CONTROL_FRONTEND_INTERNAL_H_ */
