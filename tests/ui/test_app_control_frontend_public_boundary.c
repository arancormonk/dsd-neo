// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/frontend.h>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/app_control/snapshot.h>

#if defined(DSD_NEO_INCLUDE_DSD_NEO_CORE_OPTS_H_H)
#error "public frontend app-control headers must not include core/opts.h"
#endif

#if defined(DSD_NEO_INCLUDE_DSD_NEO_CORE_STATE_H_H)
#error "public frontend app-control headers must not include core/state.h"
#endif

int
main(void) {
    (void)sizeof(dsd_frontend_status);
    (void)sizeof(dsd_frontend_metrics);
    (void)sizeof(dsd_frontend_snapshot);
    (void)sizeof(dsd_app_endpoint_payload);
    (void)&dsd_app_frontend_runtime_start;
    (void)&dsd_app_frontend_runtime_stop;
    return 0;
}
