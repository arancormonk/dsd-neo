// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_APP_CONTROL_KEY_COMMANDS_H
#define DSD_NEO_APP_CONTROL_KEY_COMMANDS_H
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
struct dsd_app_command;
int dsd_app_apply_decryption(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* command);
void dsd_app_publish_decryption_result(const struct dsd_app_command* command, int status);
#endif
