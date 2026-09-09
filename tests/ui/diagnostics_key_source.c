// SPDX-License-Identifier: GPL-3.0-or-later
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/core/safe_api.h>
#include <string.h>
#include "../../src/app_control/commands_internal.h"
int dsd_test_diagnostics_direct_key(dsd_opts* opts, dsd_state* state, const char* value);

int
dsd_test_diagnostics_direct_key(dsd_opts* opts, dsd_state* state, const char* value) {
    dsd_app_key_direct_payload payload = {0};
    payload.key_type = DSD_APP_KEY_TYPE_HEX;
    const size_t len = strlen(value);
    if (len >= sizeof payload.value) {
        return 0;
    }
    memcpy(payload.value, value, len);
    const int queued = dsd_app_command_submit(DSD_APP_CMD_KEY_DIRECT_SET, &payload, sizeof payload);
    DSD_SECURE_ZERO(&payload, sizeof payload);
    return queued > 0 && dsd_app_drain_cmds(opts, state) == 1;
}
