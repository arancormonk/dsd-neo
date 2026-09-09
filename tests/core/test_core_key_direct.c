// SPDX-License-Identifier: GPL-3.0-or-later
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/bootstrap.h>
#include <dsd-neo/runtime/cli.h>
#include <stdio.h>
#include <string.h>
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

#ifdef DSD_NEO_TEST_ARGV_FREE_WRAP
static void* watched_argv[8];
static size_t watched_sizes[8];
static int watched_frees;
// GNU linker wrapper observes storage while still allocated; never prints bytes.
// NOLINTBEGIN(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
void __real_free(void* ptr);
void __wrap_free(void* ptr);

void
__wrap_free(void* ptr) {
    for (size_t i = 0; i < 8; ++i) {
        if (ptr && ptr == watched_argv[i]) {
            const unsigned char* bytes = ptr;
            int clear = 1;
            for (size_t j = 0; j < watched_sizes[i]; ++j) {
                clear &= bytes[j] == 0;
            }
            check(clear, "argv allocation securely erased before free");
            watched_argv[i] = NULL;
            ++watched_frees;
        }
    }
    __real_free(ptr);
}

// NOLINTEND(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
#endif

static void
seed(dsd_opts* opts, dsd_state* state) {
    initOpts(opts);
    initState(state);
    dsd_app_frontend_runtime_start(opts, state);
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

static void
bootstrap_snapshot_secrets(void) {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    char name[] = "test", keyflag[] = "-H", key[65], replay[] = "-r", file[] = "playback-marker.amb";
    DSD_MEMSET(key, '1', 64);
    key[64] = 0;
    char* argv[] = {name, keyflag, key, replay, file, NULL};
    int count = 0, exit_code = -1;
    check(dsd_runtime_bootstrap(5, argv, &opts, &state, &count, &exit_code) == DSD_BOOTSTRAP_CONTINUE,
          "full bootstrap accepted");
    check(state.K1 != 0 && state.K4 != 0, "bootstrap installed key");
    int absent = 1;
    for (int i = 0; i < state.cli_argc_effective; ++i) {
        if (strstr(state.cli_argv[i], key)) {
            absent = 0;
        }
    }
    check(absent, "retained argv contains no startup key");
    check(count == 5 && state.optind == 4 && strcmp(state.cli_argv[state.optind], file) == 0,
          "playback argument index preserved");
    dsd_app_frontend_runtime_start(&opts, &state);
    const dsd_state* snapshot = dsd_app_get_latest_snapshot();
    check(snapshot && !snapshot->cli_argv && snapshot->cli_argc_effective == 0, "snapshot excludes argv ownership");
    dsd_app_frontend_runtime_stop();
#ifdef DSD_NEO_TEST_ARGV_FREE_WRAP
    for (int i = 0; i < state.cli_argc_effective && i < 8; ++i) {
        watched_argv[i] = state.cli_argv[i];
        watched_sizes[i] = strlen(state.cli_argv[i]) + 1;
    }
#endif
    freeState(&state);
#ifdef DSD_NEO_TEST_ARGV_FREE_WRAP
    check(watched_frees == count, "teardown released every retained argv allocation");
#endif
    check(!state.cli_argv && state.cli_argc_effective == 0, "teardown clears argv ownership");
    snapshot = dsd_app_get_latest_snapshot();
    check(snapshot && !snapshot->cli_argv, "retained snapshot has no dangling argv");
    DSD_SECURE_ZERO(key, sizeof key);
    DSD_SECURE_ZERO(&state, sizeof state);
    DSD_SECURE_ZERO(&opts, sizeof opts);
}

int
main(void) {
    bootstrap_snapshot_secrets();
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
