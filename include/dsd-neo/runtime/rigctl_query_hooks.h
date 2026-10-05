// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Runtime hook table for optional rigctl queries.
 *
 * Protocol code should not depend on IO headers directly. The engine installs
 * real hook functions at startup; without an installed query hook, the
 * current frequency is reported as unknown (zero).
 */
#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_RIGCTL_QUERY_HOOKS_H_
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_RIGCTL_QUERY_HOOKS_H_

#include <dsd-neo/core/opts_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The passband dsd_rigctl_query_hook_get_passband_hz() reports when the rigctl peer's is not known. */
#define DSD_RIGCTL_PASSBAND_UNKNOWN INT32_MIN

typedef struct {
    long int (*get_current_freq_hz)(const dsd_opts* opts);
    /* The passband the rigctl peer runs as far as the client knows, without I/O: Hz, 0 for the peer's own, or
       DSD_RIGCTL_PASSBAND_UNKNOWN. */
    int32_t (*get_passband_hz)(const dsd_opts* opts);
} dsd_rigctl_query_hooks;

void dsd_rigctl_query_hooks_set(dsd_rigctl_query_hooks hooks);

long int dsd_rigctl_query_hook_get_current_freq_hz(const dsd_opts* opts);

/**
 * @brief The passband the rigctl peer runs as far as the client knows (issue #628): Hz, 0 for the peer's own (and with
 * no rigctl, or no hook installed), or DSD_RIGCTL_PASSBAND_UNKNOWN when a request's reply was lost.
 */
int32_t dsd_rigctl_query_hook_get_passband_hz(const dsd_opts* opts);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_RIGCTL_QUERY_HOOKS_H_ */
