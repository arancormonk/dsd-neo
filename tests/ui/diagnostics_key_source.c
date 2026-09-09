// SPDX-License-Identifier: GPL-3.0-or-later
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <string.h>
#include "../../src/app_control/commands_internal.h"
int dsd_test_diagnostics_direct_key_refused(dsd_opts* opts, dsd_state* state, const char* value);

int
dsd_test_diagnostics_direct_key_refused(dsd_opts* opts, dsd_state* state, const char* value) {
    dsd_app_key_direct_payload payload = {0};
    payload.key_type = DSD_APP_KEY_TYPE_HEX;
    const size_t len = strlen(value);
    if (len >= sizeof payload.value) {
        return 0;
    }
    dsd_key_set before = {0};
    dsd_key_set after = {0};
    if (dsd_key_set_capture(&before, state) != 0) {
        return 0;
    }
    state->ui_msg[0] = '\0';
    const int mute_l = opts->dmr_mute_encL;
    const int mute_r = opts->dmr_mute_encR;
    DSD_MEMCPY(payload.value, value, len);
    const int queued = dsd_app_command_submit(DSD_APP_CMD_KEY_DIRECT_SET, &payload, sizeof payload);
    DSD_SECURE_ZERO(&payload, sizeof payload);
    const int drained = dsd_app_drain_cmds(opts, state);
    // Queue drainage alone says nothing about application. Require the explicit
    // WP0 refusal and unchanged key state; a real handler must break this test.
    const int refused = queued > 0 && drained == 1 && strcmp(state->ui_msg, "not implemented") == 0
                        && dsd_key_set_capture(&after, state) == 0 && dsd_key_set_equal(&before, &after)
                        && opts->dmr_mute_encL == mute_l && opts->dmr_mute_encR == mute_r && opts->show_keys == 0;
    dsd_key_set_free(&before);
    dsd_key_set_free(&after);
    return refused;
}
