// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* DSD_APP_CMD_DMR_CHANNEL_SET (discussion #900 F3): its decoder-thread handler and its retained results. */

#ifndef DSD_NEO_SRC_APP_CONTROL_DMR_CHANNEL_COMMANDS_H
#define DSD_NEO_SRC_APP_CONTROL_DMR_CHANNEL_COMMANDS_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

struct dsd_app_command;

/* Apply a DSD_APP_CMD_DMR_CHANNEL_SET on the decoder thread and return its dsd_app_dmr_channel_set_code. */
int32_t dsd_app_apply_dmr_channel_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* command);

/* Retain the result of @p command when it is a DSD_APP_CMD_DMR_CHANNEL_SET (anything else is ignored): its request as
   submitted, read field by field so a short payload still names what it carried, @p code, and the channel revision of
   @p state (NULL for a request dropped before the decoder saw it: 0). Any thread; it takes only the results' own
   lock, so the queue may hold its lock. */
void dsd_app_dmr_channel_publish_result(const struct dsd_app_command* command, int32_t code, const dsd_state* state);

#endif /* DSD_NEO_SRC_APP_CONTROL_DMR_CHANNEL_COMMANDS_H */
