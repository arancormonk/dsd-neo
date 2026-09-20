// SPDX-License-Identifier: GPL-3.0-or-later
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <string.h>
#include "../../src/app_control/commands_internal.h"
// Called by the C++ diagnostics regression.
int dsd_test_diagnostics_drain(dsd_opts* opts, dsd_state* state); // NOLINT(misc-use-internal-linkage)
// NOLINTNEXTLINE(misc-use-internal-linkage) -- Called from the C++ regression.
int dsd_test_diagnostics_direct_key_applied(dsd_opts* opts, dsd_state* state, int type, const char* value);

int
dsd_test_diagnostics_direct_key_applied(dsd_opts* opts, dsd_state* state, int type, const char* value) {
    dsd_app_key_direct_payload payload = {0};
    switch (type) {
        case DSD_KEY_TYPE_BASIC: payload.key_type = DSD_APP_KEY_TYPE_BASIC; break;
        case DSD_KEY_TYPE_HEX: payload.key_type = DSD_APP_KEY_TYPE_HEX; break;
        case DSD_KEY_TYPE_RC4: payload.key_type = DSD_APP_KEY_TYPE_RC4; break;
        case DSD_KEY_TYPE_SCRAMBLER: payload.key_type = DSD_APP_KEY_TYPE_SCRAMBLER; break;
        default: return 0;
    }
    const size_t len = strlen(value);
    if (len >= sizeof payload.value) {
        return 0;
    }
    state->ui_msg[0] = '\0';
    DSD_MEMCPY(payload.value, value, len);
    const int queued = dsd_app_command_submit(DSD_APP_CMD_KEY_DIRECT_SET, &payload, sizeof payload);
    DSD_SECURE_ZERO(&payload, sizeof payload);
    const int drained = dsd_app_drain_cmds(opts, state);
    // The C++ caller checks the type-specific scalars and overlay preservation.
    return queued > 0 && drained == 1 && strcmp(state->ui_msg, "Key applied") == 0 && state->keyloader == 0
           && state->payload_keyid == 0 && state->payload_keyidR == 0 && opts->dmr_mute_encL == 0
           && opts->dmr_mute_encR == 0 && opts->unmute_encrypted_p25 == 0 && opts->show_keys == 0;
}

int
dsd_test_diagnostics_drain(dsd_opts* opts, dsd_state* state) {
    return dsd_app_drain_cmds(opts, state);
}
