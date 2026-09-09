// SPDX-License-Identifier: GPL-3.0-or-later
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/cli.h>
#include <stdio.h>
#include "../../src/app_control/commands_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

static int failures;

static void
check(int ok, const char* label) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
        ++failures;
    }
}

static void
seed(dsd_opts* opts, dsd_state* state) {
    initOpts(opts);
    initState(state);
    state->K = 17;
    state->R = 19;
    state->RR = 23;
    state->rkey_array[7] = 29;
    state->rkey_array_loaded[7] = 1;
    opts->dmr_mute_encL = opts->dmr_mute_encR = 1;
}

static void
expected(const dsd_opts* opts, const dsd_state* state, int type, size_t width) {
    check(state->rkey_array[7] == 29 && state->rkey_array_loaded[7] == 1, "overlay preserves keyring");
    check(state->K == (type == 0 ? 0 : 17), "named basic scalar");
    check(state->R == (type == 2 ? 0xAB : type == 3 ? 32767 : 19), "named R scalar");
    check(state->RR == (type == 2 ? 0xAB : 23), "named RR scalar");
    if (type == 1) {
        check(state->K1 == (width == 10 ? 0x1111111111ULL : 0x1111111111111111ULL), "hex first scalar");
        check(state->K2 == (width == 10 ? 0 : 0x1111111111111111ULL), "hex second scalar");
        check(state->K3 == (width == 64 ? 0x1111111111111111ULL : 0), "hex third scalar");
        check(state->K4 == state->K3 && state->H == state->K1, "hex fourth and H scalar");
        check(state->aes_key_segments[0] == (width == 10 ? 0 : width / 16), "AES width");
        check(state->aes_key_loaded[0] == (width != 10), "AES activation");
    }
    check(state->keyloader == 0 && state->payload_keyid == 0 && state->payload_keyidR == 0, "direct activation");
    check(opts->dmr_mute_encL == 0 && opts->dmr_mute_encR == 0, "every value arms decryption");
}

int
main(void) {
    static dsd_state state;
    static dsd_opts opts;
    const int types[] = {0, 1, 1, 1, 2, 3};
    const size_t widths[] = {1, 10, 32, 64, 2, 5};
    const char* flags[] = {"-b", "-H", "-H", "-H", "-1", "-R"};
    for (size_t i = 0; i < 6; ++i) {
        char value[72] = {0};
        if (types[i] == 1) {
            DSD_MEMSET(value, '1', widths[i]);
        } else {
            DSD_SNPRINTF(value, sizeof value, "%s", types[i] == 0 ? "0" : types[i] == 2 ? "AB" : "32767");
        }
        for (int path = 0; path < 3; ++path) {
            seed(&opts, &state);
            if (path == 0) {
                check(dsd_key_apply_direct(&state, (dsd_key_type)types[i], value, DSD_KEY_APPLY_OVERLAY)
                          == DSD_KEY_DIRECT_OK,
                      "helper accepts type");
                dsd_key_apply_mute_policy(&opts, &state);
            } else if (path == 1) {
                char name[] = "test";
                char flag[3];
                DSD_SNPRINTF(flag, sizeof flag, "%s", flags[i]);
                char* argv[] = {name, flag, value, NULL};
                check(dsd_parse_args(3, argv, &opts, &state, NULL, NULL) == DSD_PARSE_CONTINUE, "CLI accepts type");
            } else {
                dsd_app_key_direct_payload payload = {0};
                payload.key_type = types[i];
                DSD_SNPRINTF(payload.value, sizeof payload.value, "%s", value);
                check(dsd_app_command_submit(DSD_APP_CMD_KEY_DIRECT_SET, &payload, sizeof payload)
                          == DSD_APP_COMMAND_SUBMIT_QUEUED,
                      "queue accepts type");
                DSD_SECURE_ZERO(&payload, sizeof payload);
                check(dsd_app_drain_cmds(&opts, &state) == 1, "queue drains type");
            }
            expected(&opts, &state, types[i], widths[i]);
            check(dsd_key_apply_direct(&state, (dsd_key_type)types[i], value, DSD_KEY_APPLY_REPLACE)
                      == DSD_KEY_DIRECT_OK,
                  "replace accepts each type");
            check(state.rkey_array[7] == 0 && state.rkey_array_loaded[7] == 0, "each replacement clears keyring");
            check(state.K == 0 && (types[i] == 2 || types[i] == 3 || state.R == 0),
                  "replacement clears unrelated scalars");
            freeState(&state);
        }
        DSD_SECURE_ZERO(value, sizeof value);
    }
    // Every width accepts zero through startup and live entry, with the same mute rule.
    for (size_t i = 0; i < 6; ++i) {
        for (int path = 0; path < 2; ++path) {
            seed(&opts, &state);
            dsd_app_key_direct_payload zero = {0};
            zero.key_type = types[i];
            DSD_MEMSET(zero.value, '0', types[i] == 1 ? widths[i] : 1);
            if (path == 0) {
                char name[] = "test";
                char flag[3];
                DSD_SNPRINTF(flag, sizeof flag, "%s", flags[i]);
                char* argv[] = {name, flag, zero.value, NULL};
                check(dsd_parse_args(3, argv, &opts, &state, NULL, NULL) == DSD_PARSE_CONTINUE, "zero CLI accepted");
            } else {
                dsd_app_command_submit(DSD_APP_CMD_KEY_DIRECT_SET, &zero, sizeof zero);
                dsd_app_drain_cmds(&opts, &state);
            }
            check(opts.dmr_mute_encL == 0 && opts.dmr_mute_encR == 0, "all zero types share mute policy");
            check((types[i] != 0 || state.K == 0) && (types[i] != 1 || state.K1 == 0) && (types[i] < 2 || state.R == 0),
                  "zero scalar installed");
            DSD_SECURE_ZERO(&zero, sizeof zero);
            freeState(&state);
        }
    }
    seed(&opts, &state);
    check(dsd_key_apply_direct(&state, DSD_KEY_TYPE_BASIC, "256", DSD_KEY_APPLY_REPLACE) == DSD_KEY_DIRECT_INVALID_DEC,
          "bad decimal reason");
    check(state.K == 17 && state.rkey_array[7] == 29, "failure is atomic");
    check(dsd_key_apply_direct(&state, DSD_KEY_TYPE_BASIC, "0", DSD_KEY_APPLY_REPLACE) == DSD_KEY_DIRECT_OK,
          "replace accepted");
    check(state.K == 0 && state.R == 0 && state.RR == 0 && state.rkey_array_loaded[7] == 0 && state.rkey_array[7] == 0,
          "replace clears other keys");
    dsd_key_set row = {0};
    check(dsd_key_set_load_direct(&row, NULL, "51") == DSD_KEY_DIRECT_OK, "row prepared");
    row.keyloader = 1;
    check(dsd_scan_keys_enter(&state, &row) == 1, "keyed row entered");
    dsd_app_key_direct_payload payload = {DSD_APP_KEY_TYPE_BASIC, "73"};
    dsd_app_command_submit(DSD_APP_CMD_KEY_DIRECT_SET, &payload, sizeof payload);
    DSD_SECURE_ZERO(&payload, sizeof payload);
    dsd_app_drain_cmds(&opts, &state);
    check(state.K == 51 && state.scan_keys_baseline.scalars.K == 73, "live change updates baseline under row");
    check(state.keyloader == 1, "live scalar edit preserves active keyloader");
    dsd_scan_keys_leave(&state);
    check(state.K == 73, "unkeyed hop restores updated baseline");
    dsd_scan_keys_enter(&state, &row);
    check(state.K == 51, "keyed hop restores row");
    dsd_scan_keys_leave(&state);
    check(state.K == 73, "second unkeyed hop retains change");
    dsd_key_set_free(&row);
    freeState(&state);
    for (int mode = 0; mode < 3; ++mode) {
        for (int path = 0; path < 3; ++path) {
            seed(&opts, &state);
            if (path == 0) {
                check(dsd_key_apply_force(&state, mode) == DSD_KEY_DIRECT_OK, "force helper");
            } else if (path == 1) {
                char name[] = "test";
                char flag[] = "-4";
                if (mode == 2) {
                    flag[1] = '0';
                }
                char* argv[] = {name, mode ? flag : NULL, NULL};
                check(dsd_parse_args(mode ? 2 : 1, argv, &opts, &state, NULL, NULL) == DSD_PARSE_CONTINUE, "force CLI");
            } else {
                dsd_app_command_set_i32(DSD_APP_CMD_FORCE_KEY_SET, mode);
                dsd_app_drain_cmds(&opts, &state);
            }
            check(state.M == (mode == 2 ? 0x21 : mode), "force expected state");
            freeState(&state);
        }
    }
    return failures ? 1 : 0;
}
