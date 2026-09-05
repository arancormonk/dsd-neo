// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief App-control command dispatch registry and handler signature.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_COMMAND_DISPATCH_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_COMMAND_DISPATCH_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The largest payload is a whole dsdneoUserConfig (16,432 bytes once the four [trunking]
 * CSV paths are counted); app_command_queue.c static-asserts that it and the RadioReference
 * apply payload fit. Keep headroom so the next 1 KiB path field does not need another bump. */
enum { DSD_APP_CMD_DISPATCH_DATA_MAX = 20480 };

/**
 * @brief Internal command payload envelope consumed by app-control dispatchers.
 */
struct dsd_app_command {
    size_t n;
    int id;
    uint8_t payload_truncated;
    uint8_t data[DSD_APP_CMD_DISPATCH_DATA_MAX];
};

/** Common handler signature for UI command actions. */
typedef int (*dsd_app_command_handler)(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* cmd);

/** Registry entry mapping command id -> handler function. */
struct dsd_app_command_reg {
    int id;
    dsd_app_command_handler fn;
};

/** Per-domain registries (terminated with `{0, NULL}`). */
extern const struct dsd_app_command_reg dsd_app_actions_audio[];
extern const struct dsd_app_command_reg dsd_app_actions_radio[];
extern const struct dsd_app_command_reg dsd_app_actions_trunk[];
extern const struct dsd_app_command_reg dsd_app_actions_logging[];

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_COMMAND_DISPATCH_H_ */
