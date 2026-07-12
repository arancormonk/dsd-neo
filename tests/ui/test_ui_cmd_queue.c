// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Deterministic queue-level contracts for app-control commands.
 */

#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/core/frontend_types.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../src/app_control/commands_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
static int g_io_control_tune_result = RTL_STREAM_TUNE_OK;
static int g_io_control_tune_calls = 0;
static long int g_io_control_tune_freq = 0;
static dsd_trunk_tune_result g_cc_tune_result = DSD_TRUNK_TUNE_RESULT_OK;
static int g_cc_tune_calls = 0;
static long int g_cc_tune_freq = 0;
static int g_cc_tune_ted_sps = 0;
static int g_cc_profile_at_tune = -1;

// GNU ld --wrap entry points must keep the reserved __wrap_* symbol name.
// NOLINTBEGIN(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
int __wrap_io_control_set_freq(dsd_opts* opts, dsd_state* state, long int freq);
dsd_trunk_tune_result __wrap_dsd_trunk_tuning_hook_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq,
                                                              int ted_sps);

int
__wrap_io_control_set_freq(dsd_opts* opts, dsd_state* state, long int freq) {
    (void)opts;
    (void)state;
    g_io_control_tune_calls++;
    g_io_control_tune_freq = freq;
    return g_io_control_tune_result;
}

dsd_trunk_tune_result
__wrap_dsd_trunk_tuning_hook_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps) {
    (void)opts;
    g_cc_tune_calls++;
    g_cc_tune_freq = freq;
    g_cc_tune_ted_sps = ted_sps;
    g_cc_profile_at_tune = state ? state->sps_hunt_idx : -1;
    return g_cc_tune_result;
}

// NOLINTEND(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)

static void
reset_io_control_tune_stub(int result) {
    g_io_control_tune_result = result;
    g_io_control_tune_calls = 0;
    g_io_control_tune_freq = 0;
}

static void
reset_cc_tune_stub(dsd_trunk_tune_result result) {
    g_cc_tune_result = result;
    g_cc_tune_calls = 0;
    g_cc_tune_freq = 0;
    g_cc_tune_ted_sps = 0;
    g_cc_profile_at_tune = -1;
}
#endif

static int
expect_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_u64(const char* tag, uint64_t got, uint64_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %llu want %llu\n", tag, (unsigned long long)got, (unsigned long long)want);
        return 1;
    }
    return 0;
}

static int
expect_command_status(const char* tag, dsd_app_command_token token, dsd_app_command_result_status want) {
    dsd_app_command_result result;
    DSD_MEMSET(&result, 0, sizeof(result));
    if (dsd_app_command_result_get(token, &result) != 0) {
        DSD_FPRINTF(stderr, "%s: result for token %llu not found\n", tag, (unsigned long long)token);
        return 1;
    }
    if (result.status != want) {
        DSD_FPRINTF(stderr, "%s: got status %d want %d (%s)\n", tag, (int)result.status, (int)want, result.message);
        return 1;
    }
    return 0;
}

static int
expect_command_result_stable(const char* tag, dsd_app_command_token token, dsd_app_command_result_status want) {
    dsd_app_command_result first;
    dsd_app_command_result second;
    DSD_MEMSET(&first, 0, sizeof(first));
    DSD_MEMSET(&second, 0, sizeof(second));
    if (dsd_app_command_result_get(token, &first) != 0 || dsd_app_command_result_get(token, &second) != 0) {
        DSD_FPRINTF(stderr, "%s: result for token %llu not found\n", tag, (unsigned long long)token);
        return 1;
    }
    if (first.status != want) {
        DSD_FPRINTF(stderr, "%s: got status %d want %d (%s)\n", tag, (int)first.status, (int)want, first.message);
        return 1;
    }
    if (first.token != second.token || first.coalesced_to != second.coalesced_to
        || first.command_id != second.command_id || first.status != second.status
        || first.detail_code != second.detail_code || strcmp(first.message, second.message) != 0) {
        DSD_FPRINTF(stderr, "%s: repeated result reads were not stable\n", tag);
        return 1;
    }
    return 0;
}

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "%s: expectation failed\n", tag);
        return 1;
    }
    return 0;
}

static size_t
expected_descriptor_payload_size(const dsd_app_command_descriptor* desc) {
    switch (desc->payload_kind) {
        case DSD_APP_COMMAND_PAYLOAD_NONE: return 0U;
        case DSD_APP_COMMAND_PAYLOAD_I32: return sizeof(int32_t);
        case DSD_APP_COMMAND_PAYLOAD_U8: return sizeof(uint8_t);
        case DSD_APP_COMMAND_PAYLOAD_U32: return sizeof(uint32_t);
        case DSD_APP_COMMAND_PAYLOAD_U64: return sizeof(uint64_t);
        case DSD_APP_COMMAND_PAYLOAD_DOUBLE: return sizeof(double);
        case DSD_APP_COMMAND_PAYLOAD_FLOAT: return sizeof(float);
        case DSD_APP_COMMAND_PAYLOAD_STRING: return desc->payload_size;
        case DSD_APP_COMMAND_PAYLOAD_ENDPOINT:
            if (desc->command_id == DSD_APP_CMD_UDP_INPUT_CFG) {
                const size_t udp_size = sizeof(dsd_app_udp_input_payload);
                return udp_size;
            }
            return sizeof(dsd_app_endpoint_payload);
        case DSD_APP_COMMAND_PAYLOAD_STRUCT:
            switch (desc->command_id) {
                case DSD_APP_CMD_P25_P2_PARAMS_SET: return sizeof(dsd_app_p25_p2_params_payload);
                case DSD_APP_CMD_KEY_HYTERA_SET: return sizeof(dsd_app_hytera_key_payload);
                case DSD_APP_CMD_KEY_AES_SET: return sizeof(dsd_app_aes_key_payload);
                case DSD_APP_CMD_DSP_OP: return sizeof(dsd_app_dsp_payload);
                case DSD_APP_CMD_CONFIG_APPLY: return sizeof(dsdneoUserConfig);
                case DSD_APP_CMD_CONFIG_METADATA_SET: return sizeof(dsd_app_config_metadata_payload);
                default: return 0U;
            }
        default: return 0U;
    }
}

static unsigned int
expected_descriptor_capability(dsd_app_command_payload_kind kind) {
    switch (kind) {
        case DSD_APP_COMMAND_PAYLOAD_NONE: return DSD_APP_COMMAND_CAP_ACTION;
        case DSD_APP_COMMAND_PAYLOAD_I32: return DSD_APP_COMMAND_CAP_I32;
        case DSD_APP_COMMAND_PAYLOAD_U8: return DSD_APP_COMMAND_CAP_U8;
        case DSD_APP_COMMAND_PAYLOAD_U32: return DSD_APP_COMMAND_CAP_U32;
        case DSD_APP_COMMAND_PAYLOAD_U64: return DSD_APP_COMMAND_CAP_U64;
        case DSD_APP_COMMAND_PAYLOAD_DOUBLE: return DSD_APP_COMMAND_CAP_DOUBLE;
        case DSD_APP_COMMAND_PAYLOAD_FLOAT: return DSD_APP_COMMAND_CAP_FLOAT;
        case DSD_APP_COMMAND_PAYLOAD_STRING: return DSD_APP_COMMAND_CAP_STRING;
        case DSD_APP_COMMAND_PAYLOAD_ENDPOINT: return DSD_APP_COMMAND_CAP_ENDPOINT;
        case DSD_APP_COMMAND_PAYLOAD_STRUCT: return DSD_APP_COMMAND_CAP_STRUCT;
        default: return 0U;
    }
}

static int
expect_descriptor_metadata(const dsd_app_command_descriptor* desc) {
    int rc = 0;
    const unsigned int known_availability = DSD_APP_COMMAND_AVAIL_RADIO | DSD_APP_COMMAND_AVAIL_REQUIRES_ACTIVE_RUNTIME;
    if (!desc) {
        return 1;
    }
    if (!desc->name || desc->name[0] == '\0' || !desc->label || desc->label[0] == '\0' || !desc->description
        || desc->description[0] == '\0') {
        DSD_FPRINTF(stderr, "descriptor %d missing text metadata\n", desc->command_id);
        rc = 1;
    }
    if (desc->name && strcmp(desc->name, "app_command") == 0) {
        DSD_FPRINTF(stderr, "descriptor %d uses generic command name\n", desc->command_id);
        rc = 1;
    }
    if (desc->label && strcmp(desc->label, "App Command") == 0) {
        DSD_FPRINTF(stderr, "descriptor %d uses generic command label\n", desc->command_id);
        rc = 1;
    }
    if ((desc->availability_flags & ~known_availability) != 0U) {
        DSD_FPRINTF(stderr, "descriptor %d has unknown availability flags 0x%x\n", desc->command_id,
                    desc->availability_flags);
        rc = 1;
    }
    const unsigned int expected_cap = expected_descriptor_capability(desc->payload_kind);
    if (expected_cap == 0U || (desc->capability_flags & expected_cap) == 0U) {
        DSD_FPRINTF(stderr, "descriptor %d has mismatched payload capability 0x%x for kind %d\n", desc->command_id,
                    desc->capability_flags, (int)desc->payload_kind);
        rc = 1;
    }
    const size_t expected_size = expected_descriptor_payload_size(desc);
    if (desc->payload_kind == DSD_APP_COMMAND_PAYLOAD_STRING) {
        if (desc->payload_size == 0U) {
            DSD_FPRINTF(stderr, "descriptor %d string payload size is empty\n", desc->command_id);
            rc = 1;
        }
    } else if (desc->payload_size != expected_size) {
        DSD_FPRINTF(stderr, "descriptor %d payload size got %zu want %zu\n", desc->command_id, desc->payload_size,
                    expected_size);
        rc = 1;
    }
    if (desc->enum_option_count > 0U) {
        if (!desc->enum_options) {
            DSD_FPRINTF(stderr, "descriptor %d has enum count without enum options\n", desc->command_id);
            rc = 1;
        } else {
            for (size_t i = 0; i < desc->enum_option_count; i++) {
                if (!desc->enum_options[i].label || desc->enum_options[i].label[0] == '\0') {
                    DSD_FPRINTF(stderr, "descriptor %d enum option %zu missing label\n", desc->command_id, i);
                    rc = 1;
                }
            }
        }
    }
    return rc;
}

static const dsd_app_command_descriptor*
find_descriptor(const dsd_app_command_descriptor* descs, size_t count, int command_id) {
    for (size_t i = 0; i < count; i++) {
        if (descs[i].command_id == command_id) {
            return &descs[i];
        }
    }
    return NULL;
}

static int
expect_str(const char* tag, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "%s: got \"%s\" want \"%s\"\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_contains(const char* tag, const char* haystack, const char* needle) {
    if (!haystack || !needle || !strstr(haystack, needle)) {
        DSD_FPRINTF(stderr, "%s: \"%s\" does not contain \"%s\"\n", tag, haystack ? haystack : "(null)",
                    needle ? needle : "(null)");
        return 1;
    }
    return 0;
}

static int
write_file_bytes(const char* path, const void* data, size_t n) {
    FILE* f = dsd_fopen_private(path, "wb");
    if (!f) {
        DSD_FPRINTF(stderr, "failed to create %s\n", path);
        return 1;
    }
    int rc = 0;
    if (n > 0U && fwrite(data, 1U, n, f) != n) {
        DSD_FPRINTF(stderr, "failed to write %s\n", path);
        rc = 1;
    }
    if (fclose(f) != 0) {
        DSD_FPRINTF(stderr, "failed to close %s\n", path);
        rc = 1;
    }
    return rc;
}

static void
init_test_context(dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    initOpts(opts);
    initState(state);
    state->cli_argc_effective = 0;
    state->cli_argv = NULL;
}

static int
post_empty(int id) {
    return dsd_app_post_cmd(id, NULL, 0U);
}

static int
post_i32(int id, int32_t value) {
    return dsd_app_post_cmd(id, &value, sizeof(value));
}

static int
post_u32(int id, uint32_t value) {
    return dsd_app_post_cmd(id, &value, sizeof(value));
}

static int
post_u64(int id, uint64_t value) {
    return dsd_app_post_cmd(id, &value, sizeof(value));
}

static int
post_double(int id, double value) {
    return dsd_app_post_cmd(id, &value, sizeof(value));
}

static int
post_float(int id, float value) {
    return dsd_app_post_cmd(id, &value, sizeof(value));
}

static int
post_string(int id, const char* value) {
    return dsd_app_post_cmd(id, value, strlen(value) + 1U);
}

static int
post_host_port(int id, const char* host, int32_t port) {
    uint8_t payload[256 + sizeof(port)];
    DSD_MEMSET(payload, 0, sizeof(payload));
    DSD_SNPRINTF((char*)payload, 256U, "%s", host);
    DSD_MEMCPY(payload + 256U, &port, sizeof(port));
    return dsd_app_post_cmd(id, payload, sizeof(payload));
}

static int
post_hytera_key(uint64_t h, uint64_t k1, uint64_t k2, uint64_t k3, uint64_t k4) {
    struct {
        uint64_t H;
        uint64_t K1;
        uint64_t K2;
        uint64_t K3;
        uint64_t K4;
    } payload;

    payload.H = h;
    payload.K1 = k1;
    payload.K2 = k2;
    payload.K3 = k3;
    payload.K4 = k4;
    return dsd_app_post_cmd(DSD_APP_CMD_KEY_HYTERA_SET, &payload, sizeof(payload));
}

static int
post_aes_key(uint64_t k1, uint64_t k2, uint64_t k3, uint64_t k4) {
    struct {
        uint64_t K1;
        uint64_t K2;
        uint64_t K3;
        uint64_t K4;
    } payload;

    payload.K1 = k1;
    payload.K2 = k2;
    payload.K3 = k3;
    payload.K4 = k4;
    return dsd_app_post_cmd(DSD_APP_CMD_KEY_AES_SET, &payload, sizeof(payload));
}

static int
post_p2_params(uint64_t wacn, uint64_t sysid, uint64_t cc) {
    struct {
        uint64_t w;
        uint64_t s;
        uint64_t n;
    } payload;

    payload.w = wacn;
    payload.s = sysid;
    payload.n = cc;
    return dsd_app_post_cmd(DSD_APP_CMD_P25_P2_PARAMS_SET, &payload, sizeof(payload));
}

static int
post_call_alert_events(uint8_t events) {
    return dsd_app_post_cmd(DSD_APP_CMD_CALL_ALERT_EVENTS_SET, &events, sizeof(events));
}

static int
test_typed_command_api_wrappers(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);

    rc |= expect_int("typed action rejects setter", dsd_app_command_action(DSD_APP_CMD_GAIN_SET), -1);
    rc |= expect_int("typed i32 rejects action", dsd_app_command_set_i32(DSD_APP_CMD_TOGGLE_MUTE, 1), -1);
    rc |= expect_int("typed rtl frequency rejects i32", dsd_app_command_set_i32(DSD_APP_CMD_RTL_SET_FREQ, 1), -1);
    rc |= expect_int("typed string rejects null", dsd_app_command_set_string(DSD_APP_CMD_INPUT_WAV_SET, NULL), -1);
    rc |= expect_int("typed endpoint rejects action",
                     dsd_app_command_set_endpoint(DSD_APP_CMD_TOGGLE_MUTE, "127.0.0.1", -1), -1);
    rc |= expect_int("typed udp input rejects null", dsd_app_command_set_udp_input(NULL, 0), -1);
    rc |= expect_int("typed p25 payload rejects null", dsd_app_command_set_p25_p2_params(NULL), -1);
    rc |= expect_int("typed hytera payload rejects null", dsd_app_command_set_hytera_key(NULL), -1);
    rc |= expect_int("typed aes payload rejects null", dsd_app_command_set_aes_key(NULL), -1);
    rc |= expect_int("typed dsp payload rejects null", dsd_app_command_dsp_op(NULL), -1);
    rc |= expect_int("typed config payload rejects null", dsd_app_command_apply_config(NULL), -1);

    dsd_app_p25_p2_params_payload p2 = {0xABCDEU, 0x123U, 0x456U};
    dsd_app_hytera_key_payload hytera = {0xAU, 1U, 2U, 3U, 4U};
    dsd_app_aes_key_payload aes = {9U, 10U, 11U, 12U};
    dsd_app_dsp_payload dsp = {0};

    rc |= expect_int("typed action posts", dsd_app_command_action(DSD_APP_CMD_UI_SHOW_CHANNELS_TOGGLE), 0);
    rc |= expect_int("typed gain posts", dsd_app_command_set_i32(DSD_APP_CMD_GAIN_SET, 5), 0);
    rc |= expect_int("typed gain coalesces to latest", dsd_app_command_set_i32(DSD_APP_CMD_GAIN_SET, 9), 0);
    rc |= expect_int("typed u8 posts", dsd_app_command_set_u8(DSD_APP_CMD_CALL_ALERT_EVENTS_SET, 3U), 0);
    rc |= expect_int("typed u32 posts", dsd_app_command_set_u32(DSD_APP_CMD_TG_HOLD_SET, 2468U), 0);
    rc |=
        expect_int("typed rtl frequency u32 posts", dsd_app_command_set_u32(DSD_APP_CMD_RTL_SET_FREQ, 3000000000U), 0);
    rc |= expect_int("typed u64 posts", dsd_app_command_set_u64(DSD_APP_CMD_KEY_RC4DES_SET, 0x55U), 0);
    rc |= expect_int("typed double posts", dsd_app_command_set_double(DSD_APP_CMD_HANGTIME_SET, 3.5), 0);
    rc |= expect_int("typed float posts", dsd_app_command_set_float(DSD_APP_CMD_CONST_GATE_DELTA, 1.0f), 0);
    rc |= expect_int("typed string posts", dsd_app_command_set_string(DSD_APP_CMD_M17_USER_DATA_SET, "0,DST,SRC"), 0);
    rc |= expect_int("typed endpoint posts",
                     dsd_app_command_set_endpoint(DSD_APP_CMD_RIGCTL_CONNECT_CFG, "127.0.0.1", -1), 0);
    rc |= expect_int("typed udp input endpoint posts",
                     dsd_app_command_set_endpoint(DSD_APP_CMD_UDP_INPUT_CFG, "0.0.0.0", 7355), 0);
    rc |= expect_int("typed p25 payload posts", dsd_app_command_set_p25_p2_params(&p2), 0);
    rc |= expect_int("typed hytera payload posts", dsd_app_command_set_hytera_key(&hytera), 0);
    rc |= expect_int("typed aes payload posts", dsd_app_command_set_aes_key(&aes), 0);
    rc |= expect_int("typed dsp payload posts", dsd_app_command_dsp_op(&dsp), 0);

    rc |= expect_int("typed wrappers applied with coalescing", dsd_app_drain_cmds(&opts, &state), 15);
    rc |= expect_int("typed action toggled channels", opts.frontend_display.show_channels, 1);
    rc |= expect_int("typed gain applied latest", (int)opts.audio_gain, 9);
    rc |= expect_str("typed udp input bind copied", opts.udp_in_bindaddr, "0.0.0.0");
    rc |= expect_int("typed udp input port copied", opts.udp_in_portno, 7355);
    rc |= expect_u64("typed tg hold set", state.tg_hold, 2468ULL);
    rc |= expect_u64("typed rc4des key set", state.R, 0x55ULL);
    rc |= expect_true("typed hangtime set", opts.trunk_hangtime > 3.49 && opts.trunk_hangtime < 3.51);
    rc |= expect_str("typed m17 payload copied", state.m17dat, "0,DST,SRC");
    rc |= expect_u64("typed p2 wacn set", state.p2_wacn, 0xABCDEULL);
    rc |= expect_u64("typed p2 sysid set", state.p2_sysid, 0x123ULL);
    rc |= expect_u64("typed p2 cc set", state.p2_cc, 0x456ULL);
    rc |= expect_u64("typed aes key loaded", state.A1[0], 9ULL);
    rc |= expect_int("typed aes key load flag", state.aes_key_loaded[0], 1);

    freeState(&state);
    return rc;
}

static int
test_setter_coalescing_preserves_fifo_boundaries(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);

    dsd_app_command_token first = 0;
    dsd_app_command_token second = 0;
    rc |= expect_int("fifo first gain queued", dsd_app_command_set_i32_tracked(DSD_APP_CMD_GAIN_SET, 5, &first),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("fifo gain delta queued", dsd_app_command_set_i32(DSD_APP_CMD_GAIN_DELTA, +1), 0);
    rc |= expect_int("fifo second gain queued", dsd_app_command_set_i32_tracked(DSD_APP_CMD_GAIN_SET, 11, &second),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);

    rc |= expect_int("fifo-separated setters drain independently", dsd_app_drain_cmds(&opts, &state), 3);
    rc |= expect_int("fifo-separated setters preserve final gain", (int)opts.audio_gain, 11);
    rc |= expect_command_status("fifo first gain completed", first, DSD_APP_COMMAND_RESULT_COMPLETED);
    rc |= expect_command_status("fifo second gain completed", second, DSD_APP_COMMAND_RESULT_COMPLETED);

    freeState(&state);
    return rc;
}

static int
test_tracked_command_results(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);

    dsd_app_command_token gain_first = 0;
    dsd_app_command_token gain_second = 0;
    rc |= expect_int("tracked gain queued", dsd_app_command_set_i32_tracked(DSD_APP_CMD_GAIN_SET, 5, &gain_first),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_command_status("tracked gain first queued", gain_first, DSD_APP_COMMAND_RESULT_QUEUED);
    rc |= expect_int("tracked gain coalesced", dsd_app_command_set_i32_tracked(DSD_APP_CMD_GAIN_SET, 9, &gain_second),
                     DSD_APP_COMMAND_SUBMIT_COALESCED);
    rc |= expect_command_status("tracked gain second coalesced", gain_second, DSD_APP_COMMAND_RESULT_COALESCED);
    rc |= expect_int("tracked coalesced drain count", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("tracked coalesced latest value", (int)opts.audio_gain, 9);
    rc |= expect_command_status("tracked gain first completed", gain_first, DSD_APP_COMMAND_RESULT_COMPLETED);
    rc |= expect_command_status("tracked gain second remains coalesced", gain_second, DSD_APP_COMMAND_RESULT_COALESCED);
    rc |= expect_command_result_stable("tracked gain first stable", gain_first, DSD_APP_COMMAND_RESULT_COMPLETED);
    rc |= expect_command_result_stable("tracked gain second stable", gain_second, DSD_APP_COMMAND_RESULT_COALESCED);

    uint8_t short_payload = 0xAAU;
    dsd_app_command_token invalid = 0;
    rc |= expect_int(
        "tracked invalid raw queued",
        dsd_app_command_submit_tracked(DSD_APP_CMD_KEY_AES_SET, &short_payload, sizeof short_payload, &invalid),
        DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("tracked invalid drain count", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_command_status("tracked invalid payload result", invalid, DSD_APP_COMMAND_RESULT_INVALID_PAYLOAD);
    rc |=
        expect_command_result_stable("tracked invalid payload stable", invalid, DSD_APP_COMMAND_RESULT_INVALID_PAYLOAD);

    dsd_app_command_token unsupported = 0;
    rc |= expect_int("tracked unsupported raw queued", dsd_app_command_submit_tracked(999999, NULL, 0U, &unsupported),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("tracked unsupported drain count", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_command_status("tracked unsupported result", unsupported, DSD_APP_COMMAND_RESULT_UNSUPPORTED);
    rc |= expect_command_result_stable("tracked unsupported stable", unsupported, DSD_APP_COMMAND_RESULT_UNSUPPORTED);

    dsd_app_command_token backend_fail = 0;
    rc |=
        expect_int("tracked backend failure queued",
                   dsd_app_command_set_endpoint_tracked(DSD_APP_CMD_RIGCTL_CONNECT_CFG, "127.0.0.1", -1, &backend_fail),
                   DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("tracked backend failure drain count", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_command_status("tracked backend failure result", backend_fail, DSD_APP_COMMAND_RESULT_FAILED);
    rc |= expect_command_result_stable("tracked backend failure stable", backend_fail, DSD_APP_COMMAND_RESULT_FAILED);

    dsd_app_command_capability caps[8];
    size_t cap_count = 0;
    rc |= expect_int("capability count query", dsd_app_command_capabilities_get(NULL, 0U, &cap_count), 0);
    rc |= expect_true("capability count nonzero", cap_count > 8U);
    rc |= expect_int("capability truncated query", dsd_app_command_capabilities_get(caps, 8U, &cap_count), 1);

    dsd_app_command_descriptor descs[160];
    size_t desc_count = 0;
    rc |= expect_int("descriptor count query", dsd_app_command_descriptors_get(NULL, 0U, &desc_count), 0);
    rc |= expect_int("descriptor count parity", (int)desc_count, (int)cap_count);
    rc |= expect_true("descriptor test buffer large enough", desc_count <= sizeof descs / sizeof descs[0]);
    rc |= expect_int("descriptor full query", dsd_app_command_descriptors_get(descs, desc_count, &desc_count), 0);
    const dsd_app_command_descriptor* gain_desc = NULL;
    const dsd_app_command_descriptor* rtl_gain_desc = NULL;
    const dsd_app_command_descriptor* ppm_desc = NULL;
    const dsd_app_command_descriptor* freq_desc = NULL;
    const dsd_app_command_descriptor* input_warn_desc = NULL;
    const dsd_app_command_descriptor* hangtime_desc = NULL;
    const dsd_app_command_descriptor* bw_desc = NULL;
    const dsd_app_command_descriptor* slot_pref_desc = NULL;
    const dsd_app_command_descriptor* config_desc = NULL;
    const dsd_app_command_descriptor* metadata_desc = NULL;
    for (size_t i = 0; i < desc_count; i++) {
        rc |= expect_descriptor_metadata(&descs[i]);
        if (descs[i].command_id == DSD_APP_CMD_GAIN_SET) {
            gain_desc = &descs[i];
        } else if (descs[i].command_id == DSD_APP_CMD_RTL_SET_GAIN) {
            rtl_gain_desc = &descs[i];
        } else if (descs[i].command_id == DSD_APP_CMD_RTL_SET_PPM) {
            ppm_desc = &descs[i];
        } else if (descs[i].command_id == DSD_APP_CMD_RTL_SET_FREQ) {
            freq_desc = &descs[i];
        } else if (descs[i].command_id == DSD_APP_CMD_INPUT_WARN_DB_SET) {
            input_warn_desc = &descs[i];
        } else if (descs[i].command_id == DSD_APP_CMD_HANGTIME_SET) {
            hangtime_desc = &descs[i];
        } else if (descs[i].command_id == DSD_APP_CMD_RTL_SET_BW) {
            bw_desc = &descs[i];
        } else if (descs[i].command_id == DSD_APP_CMD_SLOT_PREF_SET) {
            slot_pref_desc = &descs[i];
        } else if (descs[i].command_id == DSD_APP_CMD_CONFIG_APPLY) {
            config_desc = &descs[i];
        } else if (descs[i].command_id == DSD_APP_CMD_CONFIG_METADATA_SET) {
            metadata_desc = &descs[i];
        }
    }
    rc |= expect_true("gain descriptor present", gain_desc != NULL);
    if (gain_desc) {
        rc |= expect_true("gain descriptor range", gain_desc->min_value == 0.0 && gain_desc->max_value == 50.0);
        rc |= expect_true("gain descriptor step", gain_desc->step_value == 1.0);
    }
    rc |= expect_true("rtl gain descriptor present", rtl_gain_desc != NULL);
    if (rtl_gain_desc) {
        rc |= expect_true("rtl gain descriptor range",
                          rtl_gain_desc->min_value == 0.0 && rtl_gain_desc->max_value == 49.0);
        rc |= expect_true("rtl gain descriptor step", rtl_gain_desc->step_value == 1.0);
        rc |= expect_int("rtl gain restart hint", rtl_gain_desc->may_require_restart, 1);
    }
    rc |= expect_true("rtl ppm descriptor present", ppm_desc != NULL);
    if (ppm_desc) {
        rc |= expect_true("rtl ppm descriptor range", ppm_desc->min_value == -200.0 && ppm_desc->max_value == 200.0);
        rc |= expect_true("rtl ppm descriptor units", ppm_desc->units != NULL && strcmp(ppm_desc->units, "ppm") == 0);
    }
    rc |= expect_true("rtl frequency descriptor present", freq_desc != NULL);
    if (freq_desc) {
        rc |= expect_int("rtl frequency descriptor payload kind", freq_desc->payload_kind, DSD_APP_COMMAND_PAYLOAD_U32);
        rc |= expect_true("rtl frequency descriptor range",
                          freq_desc->min_value == 0.0 && freq_desc->max_value == 3000000000.0);
        rc |= expect_true("rtl frequency descriptor range fits u32", freq_desc->max_value <= (double)UINT32_MAX);
        rc |= expect_true("rtl frequency descriptor units",
                          freq_desc->units != NULL && strcmp(freq_desc->units, "Hz") == 0);
    }
    rc |= expect_true("input warn descriptor present", input_warn_desc != NULL);
    if (input_warn_desc) {
        rc |= expect_true("input warn descriptor range",
                          input_warn_desc->min_value == -120.0 && input_warn_desc->max_value == 0.0);
        rc |= expect_true("input warn descriptor units",
                          input_warn_desc->units != NULL && strcmp(input_warn_desc->units, "dBFS") == 0);
    }
    rc |= expect_true("hangtime descriptor present", hangtime_desc != NULL);
    if (hangtime_desc) {
        rc |= expect_true("hangtime descriptor range",
                          hangtime_desc->min_value == 0.0 && hangtime_desc->max_value == 3600.0);
        rc |= expect_true("hangtime descriptor units",
                          hangtime_desc->units != NULL && strcmp(hangtime_desc->units, "seconds") == 0);
    }
    rc |= expect_true("rtl bandwidth descriptor present", bw_desc != NULL);
    if (bw_desc) {
        rc |= expect_true("rtl bandwidth enum options", bw_desc->enum_option_count >= 7U);
        rc |= expect_true("rtl bandwidth radio availability",
                          (bw_desc->availability_flags & DSD_APP_COMMAND_AVAIL_RADIO) != 0U);
        rc |= expect_int("rtl bandwidth restart hint", bw_desc->may_require_restart, 1);
    }
    rc |= expect_true("slot preference descriptor present", slot_pref_desc != NULL);
    if (slot_pref_desc) {
        rc |= expect_true("slot preference enum option count", slot_pref_desc->enum_option_count == 3U);
        if (slot_pref_desc->enum_options && slot_pref_desc->enum_option_count == 3U) {
            rc |= expect_int("slot preference option slot1", slot_pref_desc->enum_options[0].value, 0);
            rc |= expect_str("slot preference option slot1 label", slot_pref_desc->enum_options[0].label, "Slot 1");
            rc |= expect_int("slot preference option slot2", slot_pref_desc->enum_options[1].value, 1);
            rc |= expect_str("slot preference option slot2 label", slot_pref_desc->enum_options[1].label, "Slot 2");
            rc |= expect_int("slot preference option auto", slot_pref_desc->enum_options[2].value, 2);
            rc |= expect_str("slot preference option auto label", slot_pref_desc->enum_options[2].label, "Auto");
        }
    }
    rc |= expect_true("config descriptor present", config_desc != NULL);
    if (config_desc) {
        rc |= expect_int("config restart hint", config_desc->may_require_restart, 1);
    }
    rc |= expect_true("config metadata descriptor present", metadata_desc != NULL);
    if (metadata_desc) {
        rc |= expect_int("config metadata payload kind", metadata_desc->payload_kind, DSD_APP_COMMAND_PAYLOAD_STRUCT);
        rc |= expect_true("config metadata payload size",
                          metadata_desc->payload_size == sizeof(dsd_app_config_metadata_payload));
    }
    rc |= expect_true("descriptor lookup helper",
                      find_descriptor(descs, desc_count, DSD_APP_CMD_RTL_SET_AUTO_PPM) != NULL);

    dsd_app_config_metadata_payload metadata;
    DSD_MEMSET(&metadata, 0, sizeof metadata);
    metadata.autosave_enabled = 1;
    DSD_SNPRINTF(metadata.path, sizeof metadata.path, "%s", "/tmp/queued-config.toml");
    dsd_app_command_token metadata_token = 0;
    rc |= expect_int("tracked config metadata queued",
                     dsd_app_command_set_config_metadata_tracked(&metadata, &metadata_token),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("tracked config metadata drain count", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("tracked config metadata enabled", state.config_autosave_enabled, 1);
    rc |= expect_str("tracked config metadata path", state.config_autosave_path, "/tmp/queued-config.toml");
    rc |= expect_command_status("tracked config metadata result", metadata_token, DSD_APP_COMMAND_RESULT_COMPLETED);
    rc |= expect_command_result_stable("tracked config metadata stable", metadata_token,
                                       DSD_APP_COMMAND_RESULT_COMPLETED);

    opts.frontend_kind = DSD_FRONTEND_TERMINAL;
    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.frontend_kind = DSD_FRONTEND_NATIVE;
    cfg.frontend_kind_is_set = 1;
    dsd_app_command_token restart = 0;
    rc |= expect_int("tracked config restart queued", dsd_app_command_apply_config_tracked(&cfg, &restart),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("tracked config restart drain count", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("tracked config preserves active frontend", opts.frontend_kind, DSD_FRONTEND_TERMINAL);
    rc |= expect_command_status("tracked config restart result", restart, DSD_APP_COMMAND_RESULT_RESTART_REQUIRED);
    rc |=
        expect_command_result_stable("tracked config restart stable", restart, DSD_APP_COMMAND_RESULT_RESTART_REQUIRED);

    freeState(&state);
    return rc;
}

static int
test_visibility_and_queue_overflow(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);

    rc |= expect_int("initial queue empty", dsd_app_drain_cmds(&opts, &state), 0);

    post_empty(DSD_APP_CMD_UI_SHOW_DSP_PANEL_TOGGLE);
    post_empty(DSD_APP_CMD_UI_SHOW_P25_METRICS_TOGGLE);
    post_empty(DSD_APP_CMD_UI_SHOW_P25_AFFIL_TOGGLE);
    post_empty(DSD_APP_CMD_UI_SHOW_P25_NEIGHBORS_TOGGLE);
    post_empty(DSD_APP_CMD_UI_SHOW_P25_IDEN_TOGGLE);
    post_empty(DSD_APP_CMD_UI_SHOW_P25_CCC_TOGGLE);
    post_empty(DSD_APP_CMD_UI_SHOW_CHANNELS_TOGGLE);
    post_empty(DSD_APP_CMD_UI_SHOW_P25_CALLSIGN_TOGGLE);
    rc |= expect_int("visibility commands applied", dsd_app_drain_cmds(&opts, &state), 8);
    rc |= expect_int("dsp panel visible", opts.frontend_display.show_dsp_panel, 1);
    rc |= expect_int("p25 metrics visible", opts.frontend_display.show_p25_metrics, 1);
    rc |= expect_int("p25 affiliations visible", opts.frontend_display.show_p25_affiliations, 1);
    rc |= expect_int("p25 neighbors visible", opts.frontend_display.show_p25_neighbors, 1);
    rc |= expect_int("p25 iden visible", opts.frontend_display.show_p25_iden_plan, 1);
    rc |= expect_int("p25 candidates visible", opts.frontend_display.show_p25_cc_candidates, 1);
    rc |= expect_int("channels visible", opts.frontend_display.show_channels, 1);
    rc |= expect_int("callsign visible", opts.frontend_display.show_p25_callsign_decode, 1);

    opts.frontend_display.show_channels = 0;
    for (int i = 0; i < 140; i++) {
        post_empty(DSD_APP_CMD_UI_SHOW_CHANNELS_TOGGLE);
    }
    rc |= expect_int("overflow keeps bounded queue depth", dsd_app_drain_cmds(&opts, &state), 127);
    rc |= expect_int("overflow drains newest visibility toggles", opts.frontend_display.show_channels, 1);

    freeState(&state);
    return rc;
}

static int
test_key_and_runtime_state_commands(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);

    opts.dmr_mute_encL = 1;
    opts.dmr_mute_encR = 1;
    state.payload_keyid = 9;
    state.payload_keyidR = 10;

    post_u32(DSD_APP_CMD_KEY_BASIC_SET, 0x12345678U);
    post_u32(DSD_APP_CMD_KEY_SCRAMBLER_SET, 0x11112222U);
    post_u64(DSD_APP_CMD_KEY_RC4DES_SET, 0x55667788ULL);
    post_hytera_key(0xAU, 1U, 2U, 0U, 0U);
    post_aes_key(3U, 4U, 5U, 6U);
    post_string(DSD_APP_CMD_M17_USER_DATA_SET, "0,DEST,SOURCE");
    post_i32(DSD_APP_CMD_RIGCTL_SET_MOD_BW, 12500);
    post_u32(DSD_APP_CMD_TG_HOLD_SET, 4567U);
    post_double(DSD_APP_CMD_HANGTIME_SET, 2.5);
    post_i32(DSD_APP_CMD_SLOT_PREF_SET, 1);
    post_i32(DSD_APP_CMD_SLOTS_ONOFF_SET, 2);
    post_p2_params(0xABCDEU, 0x123U, 0x456U);

    rc |= expect_int("key/runtime commands applied", dsd_app_drain_cmds(&opts, &state), 12);
    rc |= expect_u64("basic key loaded", state.K, 0x12345678ULL);
    rc |= expect_u64("scrambler key loaded", state.R, 0x55667788ULL);
    rc |= expect_u64("rc4des key mirror loaded", state.RR, 0x55667788ULL);
    rc |= expect_int("key mute reset left", opts.dmr_mute_encL, 0);
    rc |= expect_int("key mute reset right", opts.dmr_mute_encR, 0);
    rc |= expect_int("payload key reset left", state.payload_keyid, 0);
    rc |= expect_int("payload key reset right", state.payload_keyidR, 0);
    rc |= expect_u64("aes key loaded", state.A1[0], 3ULL);
    rc |= expect_int("aes key load flag left", state.aes_key_loaded[0], 1);
    rc |= expect_int("aes key segments left", state.aes_key_segments[0], 4);
    rc |= expect_u64("hytera state cleared by aes", state.H, 0ULL);
    rc |= expect_int("m17 user data copied", strncmp(state.m17dat, "0,DEST,SOURCE", sizeof(state.m17dat)), 0);
    rc |= expect_int("rigctl bw set", opts.setmod_bw, 12500);
    rc |= expect_u64("tg hold set", state.tg_hold, 4567ULL);
    rc |= expect_true("hangtime set", opts.trunk_hangtime > 2.49 && opts.trunk_hangtime < 2.51);
    rc |= expect_int("slot preference set", opts.slot_preference, 1);
    rc |= expect_int("slot1 disabled by mask", opts.slot1_on, 0);
    rc |= expect_int("slot2 enabled by mask", opts.slot2_on, 1);
    rc |= expect_u64("p2 wacn set", state.p2_wacn, 0xABCDEULL);
    rc |= expect_u64("p2 sysid set", state.p2_sysid, 0x123ULL);
    rc |= expect_u64("p2 cc set", state.p2_cc, 0x456ULL);

    post_i32(DSD_APP_CMD_SLOT_PREF_SET, 2);
    rc |= expect_int("slot preference auto drain count", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("slot preference auto set", opts.slot_preference, 2);
    rc |= expect_true("slot preference auto toast", strstr(state.ui_msg, "Slot preference -> Auto") != NULL);

    uint8_t short_payload = 0xFFU;
    state.K = 0x99999999U;
    state.A1[0] = 0x55U;
    post_u32(DSD_APP_CMD_KEY_BASIC_SET, 0x01020304U);
    dsd_app_post_cmd(DSD_APP_CMD_KEY_AES_SET, &short_payload, sizeof(short_payload));
    post_string(DSD_APP_CMD_M17_USER_DATA_SET, "");
    rc |= expect_int("short key payload commands applied", dsd_app_drain_cmds(&opts, &state), 3);
    rc |= expect_u64("basic key still updates before short aes", state.K, 0x01020304ULL);
    rc |= expect_u64("short aes ignored", state.A1[0], 0x55ULL);
    rc |= expect_str("empty m17 payload clears value", state.m17dat, "");

    freeState(&state);
    return rc;
}

static int
test_file_network_and_import_commands(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    const char* symbol_out = "ui_cmd_queue_symbols_out.bin";
    const char* symbol_in = "ui_cmd_queue_symbols_in.bin";
    const char* missing_csv = "ui_cmd_queue_missing.csv";
    const unsigned char symbol_data[] = {0x12U, 0x34U, 0x56U, 0x78U};

    remove(symbol_out);
    remove(symbol_in);
    remove(missing_csv);

    init_test_context(&opts, &state);

    rc |= write_file_bytes(symbol_in, symbol_data, sizeof(symbol_data));

    post_string(DSD_APP_CMD_DSP_OUT_SET, "stream.float");
    post_string(DSD_APP_CMD_SYMCAP_OPEN, symbol_out);
    post_string(DSD_APP_CMD_SYMBOL_IN_OPEN, symbol_in);
    rc |= expect_int("file command group applied", dsd_app_drain_cmds(&opts, &state), 3);
    rc |= expect_int("dsp output enabled", opts.use_dsp_output, 1);
    rc |= expect_str("dsp output path", opts.dsp_out_file, "./DSP/stream.float");
    rc |= expect_str("symbol output path", opts.symbol_out_file, symbol_out);
    rc |= expect_true("symbol output opened", opts.symbol_out_f != NULL);
    rc |= expect_true("symbol input opened", opts.symbolfile != NULL);
    rc |= expect_int("symbol input type selected", opts.audio_in_type, AUDIO_IN_SYMBOL_BIN);
    rc |= expect_int("symbol replay format reset", state.symbol_replay_format, 0);
    rc |= expect_int("symbol replay header reset", state.symbol_replay_header_checked, 0);

    if (opts.symbol_out_f) {
        fclose(opts.symbol_out_f);
        opts.symbol_out_f = NULL;
    }
    if (opts.symbolfile) {
        fclose(opts.symbolfile);
        opts.symbolfile = NULL;
    }

    post_string(DSD_APP_CMD_PULSE_OUT_SET, "sink0");
    post_string(DSD_APP_CMD_PULSE_IN_SET, "source0");
    rc |= expect_int("pulse command group applied", dsd_app_drain_cmds(&opts, &state), 2);
    rc |= expect_str("pulse output selected", opts.audio_out_dev, "pulse");
    rc |= expect_int("pulse output type selected", opts.audio_out_type, 0);
    rc |= expect_str("pulse input selected", opts.audio_in_dev, "pulse");
    rc |= expect_int("pulse input type selected", opts.audio_in_type, AUDIO_IN_PULSE);

    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "unchanged");
    opts.audio_in_type = AUDIO_IN_STDIN;
    dsd_app_post_cmd(DSD_APP_CMD_UDP_INPUT_CFG, NULL, 0U);
    rc |= expect_int("malformed udp input command applied", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("malformed udp input leaves device", opts.audio_in_dev, "unchanged");
    rc |= expect_int("malformed udp input leaves type", opts.audio_in_type, AUDIO_IN_STDIN);

    post_host_port(DSD_APP_CMD_UDP_OUT_CFG, "127.0.0.1", 0);
    post_host_port(DSD_APP_CMD_TCP_CONNECT_AUDIO_CFG, "127.0.0.1", -1);
    post_host_port(DSD_APP_CMD_RIGCTL_CONNECT_CFG, "127.0.0.1", -1);
    rc |= expect_int("network failure command group applied", dsd_app_drain_cmds(&opts, &state), 3);
    rc |= expect_contains("rigctl failure toast", state.ui_msg, "Rigctl connect failed");
    rc |= expect_int("rigctl remains disabled", opts.use_rigctl, 0);

    post_empty(DSD_APP_CMD_LRRP_SET_DSDP);
    rc |= expect_int("lrrp dsdp applied", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("lrrp dsdp path", opts.lrrp_out_file, "DSDPlus.LRRP");
    rc |= expect_int("lrrp dsdp enabled", opts.lrrp_file_output, 1);

    post_string(DSD_APP_CMD_LRRP_SET_CUSTOM, "positions.csv");
    rc |= expect_int("lrrp custom applied", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("lrrp custom path", opts.lrrp_out_file, "positions.csv");

    post_string(DSD_APP_CMD_IMPORT_CHANNEL_MAP, missing_csv);
    post_string(DSD_APP_CMD_IMPORT_GROUP_LIST, missing_csv);
    post_string(DSD_APP_CMD_IMPORT_KEYS_DEC, missing_csv);
    post_string(DSD_APP_CMD_IMPORT_KEYS_HEX, missing_csv);
    rc |= expect_int("import failure group applied", dsd_app_drain_cmds(&opts, &state), 4);
    rc |= expect_str("channel import path copied", opts.chan_in_file, missing_csv);
    rc |= expect_str("group import path copied", opts.group_in_file, missing_csv);
    rc |= expect_str("key import path copied", opts.key_in_file, missing_csv);
    rc |= expect_contains("key import failure toast", state.ui_msg, "Failed: Keys (HEX)");

    post_string(DSD_APP_CMD_EVENT_LOG_SET, "events.log");
    rc |= expect_int("event log set applied", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("event log path set", opts.event_out_file, "events.log");
    post_empty(DSD_APP_CMD_EVENT_LOG_DISABLE);
    rc |= expect_int("event log disable applied", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("event log disabled", opts.event_out_file, "");

    remove(symbol_out);
    remove(symbol_in);
    freeState(&state);
    return rc;
}

static int
test_io_and_legacy_state_commands(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    init_test_context(&opts, &state);

    opts.slot1_on = 1;
    opts.slot2_on = 1;
    opts.slot_preference = 0;
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.frontend_display.const_gate_other = 0.05f;
    opts.frontend_display.const_gate_qpsk = 0.10f;
    opts.frontend_display.const_norm_mode = 0;
    opts.frontend_display.eye_unicode = 0;
    opts.frontend_display.eye_color = 0;
    opts.use_lpf = 0;
    opts.use_hpf = 0;
    opts.use_pbf = 0;
    opts.use_hpf_d = 0;
    opts.aggressive_framesync = 0;
    opts.call_alert_events = 0xFFU;
    opts.call_alert = 0;
    opts.p25_lcw_retune = 0;
    opts.reverse_mute = 0;
    opts.inverted_x2tdma = 0;
    opts.inverted_dmr = 0;
    opts.inverted_dpmr = 0;
    opts.inverted_m17 = 0;
    opts.m17encoder = 1;
    opts.frame_provoice = 1;
    state.ea_mode = 0;

    post_empty(DSD_APP_CMD_CONST_TOGGLE);
    post_empty(DSD_APP_CMD_CONST_NORM_TOGGLE);
    post_float(DSD_APP_CMD_CONST_GATE_DELTA, 1.0f);
    post_empty(DSD_APP_CMD_EYE_TOGGLE);
    post_empty(DSD_APP_CMD_EYE_UNICODE_TOGGLE);
    post_empty(DSD_APP_CMD_EYE_COLOR_TOGGLE);
    post_empty(DSD_APP_CMD_FSK_HIST_TOGGLE);
    post_empty(DSD_APP_CMD_SPECTRUM_TOGGLE);
    post_empty(DSD_APP_CMD_TOGGLE_COMPACT);
    post_empty(DSD_APP_CMD_SLOT1_TOGGLE);
    post_empty(DSD_APP_CMD_SLOT2_TOGGLE);
    post_empty(DSD_APP_CMD_SLOT_PREF_CYCLE);
    post_empty(DSD_APP_CMD_PAYLOAD_TOGGLE);
    post_empty(DSD_APP_CMD_P25_GA_TOGGLE);
    post_empty(DSD_APP_CMD_LPF_TOGGLE);
    post_empty(DSD_APP_CMD_HPF_TOGGLE);
    post_empty(DSD_APP_CMD_PBF_TOGGLE);
    post_empty(DSD_APP_CMD_HPF_D_TOGGLE);
    post_empty(DSD_APP_CMD_AGGR_SYNC_TOGGLE);
    post_empty(DSD_APP_CMD_CALL_ALERT_TOGGLE);
    post_call_alert_events(0U);
    post_empty(DSD_APP_CMD_CRC_RELAX_TOGGLE);
    post_empty(DSD_APP_CMD_LCW_RETUNE_TOGGLE);
    post_empty(DSD_APP_CMD_P25_CC_CAND_TOGGLE);
    post_empty(DSD_APP_CMD_REVERSE_MUTE_TOGGLE);
    post_empty(DSD_APP_CMD_INV_X2_TOGGLE);
    post_empty(DSD_APP_CMD_INV_DMR_TOGGLE);
    post_empty(DSD_APP_CMD_INV_DPMR_TOGGLE);
    post_empty(DSD_APP_CMD_INV_M17_TOGGLE);
    post_string(DSD_APP_CMD_INPUT_WAV_SET, "input.wav");
    post_string(DSD_APP_CMD_INPUT_SYM_STREAM_SET, "symbols.f32");
    post_empty(DSD_APP_CMD_INPUT_SET_PULSE);
    post_host_port(DSD_APP_CMD_UDP_INPUT_CFG, "0.0.0.0", 7355);
    post_empty(DSD_APP_CMD_M17_TX_TOGGLE);
    post_empty(DSD_APP_CMD_PROVOICE_ESK_TOGGLE);
    post_empty(DSD_APP_CMD_PROVOICE_MODE_TOGGLE);
    post_empty(DSD_APP_CMD_LRRP_DISABLE);
    post_empty(DSD_APP_CMD_DMR_RESET);

    rc |= expect_int("io/legacy commands applied", dsd_app_drain_cmds(&opts, &state), 38);
    rc |= expect_str("udp input selected", opts.audio_in_dev, "udp");
    rc |= expect_int("udp input type", opts.audio_in_type, AUDIO_IN_UDP);
    rc |= expect_str("udp bind copied", opts.udp_in_bindaddr, "0.0.0.0");
    rc |= expect_int("udp port copied", opts.udp_in_portno, 7355);
    rc |= expect_int("compact toggled", opts.frontend_display.terminal_compact, 1);
    rc |= expect_int("slot1 disabled", opts.slot1_on, 0);
    rc |= expect_int("slot2 disabled", opts.slot2_on, 0);
    rc |= expect_int("slot preference cycled", opts.slot_preference, 1);
    rc |= expect_int("payload toggled", opts.payload, 1);
    rc |= expect_int("p25 ga toggled", opts.frontend_display.show_p25_group_affiliations, 1);
    rc |= expect_int("lpf toggled", opts.use_lpf, 1);
    rc |= expect_int("hpf toggled", opts.use_hpf, 1);
    rc |= expect_int("pbf toggled", opts.use_pbf, 1);
    rc |= expect_int("hpf-d toggled", opts.use_hpf_d, 1);
    rc |= expect_int("aggressive sync toggled twice", opts.aggressive_framesync, 0);
    rc |= expect_int("call alert disabled by empty event mask", opts.call_alert, 0);
    rc |= expect_int("call alert events masked to zero", opts.call_alert_events, 0);
    rc |= expect_int("lcw retune toggled", opts.p25_lcw_retune, 1);
    rc |= expect_int("candidate preference toggled", opts.p25_prefer_candidates, 1);
    rc |= expect_int("x2 inverted", opts.inverted_x2tdma, 1);
    rc |= expect_int("dmr inverted", opts.inverted_dmr, 1);
    rc |= expect_int("dpmr inverted", opts.inverted_dpmr, 1);
    rc |= expect_int("m17 inverted", opts.inverted_m17, 1);
    rc |= expect_int("constellation toggled", opts.frontend_display.constellation, 1);
    rc |= expect_int("constellation normalization toggled", opts.frontend_display.const_norm_mode, 1);
    rc |= expect_true("constellation gate clamped", opts.frontend_display.const_gate_other > 0.89f
                                                        && opts.frontend_display.const_gate_other <= 0.90f);
    rc |= expect_int("eye toggled", opts.frontend_display.eye_view, 1);
    rc |= expect_int("eye unicode toggled", opts.frontend_display.eye_unicode, 1);
    rc |= expect_int("eye color toggled", opts.frontend_display.eye_color, 1);
    rc |= expect_int("fsk histogram toggled", opts.frontend_display.fsk_hist_view, 1);
    rc |= expect_int("spectrum toggled", opts.frontend_display.spectrum_view, 1);
    rc |= expect_int("m17 tx toggled", state.m17encoder_tx, 1);
    rc |= expect_int("provoice esk toggled", state.esk_mask, 0xA0);
    rc |= expect_int("provoice mode toggled", state.ea_mode, 1);
    rc |= expect_int("lrrp disabled", opts.lrrp_file_output, 0);
    rc |= expect_int("dmr reset rest channel", state.dmr_rest_channel, -1);
    rc |= expect_int("dmr reset mfid", state.dmr_mfid, -1);

    post_empty(DSD_APP_CMD_FORCE_PRIV_TOGGLE);
    post_empty(DSD_APP_CMD_FORCE_PRIV_TOGGLE);
    post_empty(DSD_APP_CMD_FORCE_RC4_TOGGLE);
    post_empty(DSD_APP_CMD_SLOT1_TOGGLE);
    post_empty(DSD_APP_CMD_SLOT2_TOGGLE);
    post_empty(DSD_APP_CMD_SLOT_PREF_CYCLE);
    post_empty(DSD_APP_CMD_SLOT_PREF_CYCLE);
    post_empty(DSD_APP_CMD_M17_TX_TOGGLE);
    post_empty(DSD_APP_CMD_PROVOICE_ESK_TOGGLE);
    post_empty(DSD_APP_CMD_PROVOICE_MODE_TOGGLE);
    rc |= expect_int("second legacy command group applied", dsd_app_drain_cmds(&opts, &state), 10);
    rc |= expect_int("force rc4 selected", state.M, 0x21);
    rc |= expect_int("slot1 re-enabled", opts.slot1_on, 1);
    rc |= expect_int("slot2 re-enabled", opts.slot2_on, 1);
    rc |= expect_int("slot preference wrapped", opts.slot_preference, 0);
    rc |= expect_int("m17 tx toggled off sets eot", state.m17encoder_tx, 0);
    rc |= expect_int("m17 tx eot set", state.m17encoder_eot, 1);
    rc |= expect_int("provoice esk toggled back", state.esk_mask, 0);
    rc |= expect_int("provoice mode toggled back", state.ea_mode, 0);

    freeState(&state);
    return rc;
}

#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
static void
seed_active_p25_voice(dsd_opts* opts, dsd_state* state, long cc_freq, long vc_freq, int tg) {
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->p25_trunk = 1;
    opts->p25_is_tuned = 1;
    opts->trunk_is_tuned = 1;
    state->p25_cc_freq = cc_freq;
    state->trunk_cc_freq = cc_freq;
    state->p25_vc_freq[0] = state->p25_vc_freq[1] = vc_freq;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = vc_freq;
    state->lasttg = tg;
    state->lastsrc = tg + 1;
    state->last_cc_sync_time = 123;
    state->last_cc_sync_time_m = 42.0;
    state->samplesPerSymbol = 7;
    state->symbolCenter = 3;
    state->p25_cc_is_tdma = 0;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_2;
    state->sps_hunt_counter = 17;
}

static int
test_manual_tune_commands_commit_only_after_acceptance(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    dsd_app_command_token token = 0;

#ifdef USE_RADIO
    init_test_context(&opts, &state);
    reset_io_control_tune_stub(RTL_STREAM_TUNE_TIMEOUT);
    rc |= expect_int("accepted RTL frequency timeout queued",
                     dsd_app_command_set_u32_tracked(DSD_APP_CMD_RTL_SET_FREQ, 851500000U, &token),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("accepted RTL frequency timeout drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_command_status("accepted RTL frequency timeout completed", token, DSD_APP_COMMAND_RESULT_COMPLETED);
    rc |= expect_true("accepted RTL frequency timeout reports pending",
                      strstr(state.ui_msg, "Accepted: RTL frequency -> 851500000 Hz (pending)") != NULL);
    rc |= expect_int("accepted RTL frequency timeout tune calls", g_io_control_tune_calls, 1);
    freeState(&state);
#endif

    init_test_context(&opts, &state);
    seed_active_p25_voice(&opts, &state, 851000000L, 852000000L, 1201);
    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_DEFERRED);
    rc |= expect_int("deferred return-to-CC queued", dsd_app_command_action_tracked(DSD_APP_CMD_RETURN_CC, &token),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("deferred return-to-CC drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_command_status("deferred return-to-CC failed", token, DSD_APP_COMMAND_RESULT_FAILED);
    rc |= expect_int("deferred return-to-CC CC tune calls", g_cc_tune_calls, 1);
    rc |= expect_int("deferred return-to-CC raw tune calls", g_io_control_tune_calls, 0);
    rc |= expect_true("deferred return-to-CC frequency", g_cc_tune_freq == 851000000L);
    rc |= expect_int("deferred return-to-CC TED SPS", g_cc_tune_ted_sps, 10);
    rc |= expect_int("deferred return-to-CC profile staged before commit", g_cc_profile_at_tune,
                     DSD_FRAME_SYNC_SPS_PROFILE_4800_2);
    rc |= expect_int("deferred return-to-CC keeps P25 tuned", opts.p25_is_tuned, 1);
    rc |= expect_int("deferred return-to-CC keeps trunk tuned", opts.trunk_is_tuned, 1);
    rc |= expect_int("deferred return-to-CC keeps TG", state.lasttg, 1201);
    rc |= expect_true("deferred return-to-CC keeps VC", state.p25_vc_freq[0] == 852000000L);
    rc |= expect_true("deferred return-to-CC keeps CC sync", state.last_cc_sync_time_m == 42.0);
    rc |= expect_int("deferred return-to-CC keeps SPS", state.samplesPerSymbol, 7);
    rc |= expect_int("deferred return-to-CC keeps SPS profile", state.sps_hunt_idx, DSD_FRAME_SYNC_SPS_PROFILE_4800_2);
    rc |= expect_int("deferred return-to-CC keeps SPS hunt counter", state.sps_hunt_counter, 17);

    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_PENDING);
    token = 0;
    rc |= expect_int("accepted timeout return-to-CC queued",
                     dsd_app_command_action_tracked(DSD_APP_CMD_RETURN_CC, &token), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("accepted timeout return-to-CC drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_command_status("accepted timeout return-to-CC completed", token, DSD_APP_COMMAND_RESULT_COMPLETED);
    rc |= expect_int("accepted timeout clears P25 tuned", opts.p25_is_tuned, 0);
    rc |= expect_int("accepted timeout clears trunk tuned", opts.trunk_is_tuned, 0);
    rc |= expect_int("accepted timeout clears TG", state.lasttg, 0);
    rc |= expect_true("accepted timeout clears VC", state.p25_vc_freq[0] == 0L);
    rc |= expect_true("accepted timeout refreshes CC sync", state.last_cc_sync_time_m > 42.0);
    rc |= expect_int("accepted timeout selects P25 SPS profile", state.sps_hunt_idx, DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    rc |= expect_int("accepted timeout resets SPS hunt counter", state.sps_hunt_counter, 0);
    rc |= expect_int("accepted timeout used profile-aware CC tune", g_cc_tune_calls, 1);
    rc |= expect_int("accepted timeout profile was staged before commit", g_cc_profile_at_tune,
                     DSD_FRAME_SYNC_SPS_PROFILE_4800_2);
    freeState(&state);

    init_test_context(&opts, &state);
    seed_active_p25_voice(&opts, &state, 853000000L, 854000000L, 2201);
    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_DEFERRED);
    token = 0;
    rc |= expect_int("deferred lockout queued", dsd_app_command_set_u8_tracked(DSD_APP_CMD_LOCKOUT_SLOT, 0U, &token),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("deferred lockout drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_command_status("deferred lockout policy completed", token, DSD_APP_COMMAND_RESULT_COMPLETED);
    rc |= expect_int("deferred lockout tune calls", g_cc_tune_calls, 1);
    rc |= expect_int("deferred lockout keeps P25 tuned", opts.p25_is_tuned, 1);
    rc |= expect_int("deferred lockout keeps trunk tuned", opts.trunk_is_tuned, 1);
    rc |= expect_int("deferred lockout keeps TG", state.lasttg, 2201);
    rc |= expect_true("deferred lockout keeps VC", state.p25_vc_freq[0] == 854000000L);
    rc |= expect_true("deferred lockout keeps CC sync", state.last_cc_sync_time_m == 42.0);
    rc |= expect_int("deferred lockout keeps SPS", state.samplesPerSymbol, 7);
    char lockout_mode[8] = {0};
    char lockout_name[50] = {0};
    rc |= expect_int("deferred lockout policy installed",
                     dsd_tg_policy_lookup_label(&state, 2201U, lockout_mode, sizeof(lockout_mode), lockout_name,
                                                sizeof(lockout_name)),
                     1);
    rc |= expect_str("deferred lockout policy mode", lockout_mode, "B");
    rc |= expect_str("deferred lockout policy name", lockout_name, "LOCKOUT");
    rc |= expect_true("deferred lockout reports cleanup separately",
                      strstr(state.ui_msg, "TG 2201 locked out; return-to-CC tune failed") != NULL);
    freeState(&state);

    init_test_context(&opts, &state);
    seed_active_p25_voice(&opts, &state, 0L, 854500000L, 2202);
    state.carrier = 1;
    reset_io_control_tune_stub(RTL_STREAM_TUNE_DEFERRED);
    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_DEFERRED);
    token = 0;
    rc |= expect_int("no-CC lockout queued", dsd_app_command_set_u8_tracked(DSD_APP_CMD_LOCKOUT_SLOT, 0U, &token),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("no-CC lockout drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_command_status("no-CC lockout completed", token, DSD_APP_COMMAND_RESULT_COMPLETED);
    rc |= expect_int("no-CC lockout skips raw tune", g_io_control_tune_calls, 0);
    rc |= expect_int("no-CC lockout skips CC tune", g_cc_tune_calls, 0);
    rc |= expect_int("no-CC lockout clears P25 tuned", opts.p25_is_tuned, 0);
    rc |= expect_int("no-CC lockout clears trunk tuned", opts.trunk_is_tuned, 0);
    rc |= expect_int("no-CC lockout clears TG", state.lasttg, 0);
    rc |= expect_true("no-CC lockout clears P25 VC", state.p25_vc_freq[0] == 0L);
    rc |= expect_true("no-CC lockout clears trunk VC", state.trunk_vc_freq[0] == 0L);
    rc |= expect_int("no-CC lockout runs no-carrier cleanup", state.carrier, 0);
    rc |= expect_true("no-CC lockout keeps CC unknown", state.trunk_cc_freq == 0L && state.p25_cc_freq == 0L);
    freeState(&state);

    init_test_context(&opts, &state);
    seed_active_p25_voice(&opts, &state, 855000000L, 856000000L, 3201);
    state.lcn_freq_count = 4;
    state.lcn_freq_roll = 0;
    state.trunk_lcn_freq[0] = 0L;
    state.trunk_lcn_freq[1] = 857000000L;
    state.trunk_lcn_freq[2] = 0L;
    state.trunk_lcn_freq[3] = 858000000L;
    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_DEFERRED);
    token = 0;
    rc |= expect_int("deferred channel cycle queued", dsd_app_command_action_tracked(DSD_APP_CMD_CHANNEL_CYCLE, &token),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("deferred channel cycle drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_command_status("deferred channel cycle failed", token, DSD_APP_COMMAND_RESULT_FAILED);
    rc |= expect_int("deferred channel cycle tune calls", g_cc_tune_calls, 1);
    rc |= expect_true("deferred channel cycle frequency", g_cc_tune_freq == 857000000L);
    rc |= expect_int("deferred channel cycle keeps roll", state.lcn_freq_roll, 0);
    rc |= expect_int("deferred channel cycle keeps tuned", opts.p25_is_tuned, 1);
    rc |= expect_int("deferred channel cycle keeps TG", state.lasttg, 3201);
    rc |= expect_true("deferred channel cycle keeps VC", state.p25_vc_freq[0] == 856000000L);
    rc |= expect_true("deferred channel cycle keeps CC sync", state.last_cc_sync_time_m == 42.0);

    state.samplesPerSymbol = 8;
    state.symbolCenter = 3;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_6000_4;
    state.sps_hunt_counter = 23;
    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_OK);
    token = 0;
    rc |= expect_int("accepted channel cycle queued", dsd_app_command_action_tracked(DSD_APP_CMD_CHANNEL_CYCLE, &token),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("accepted channel cycle drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_command_status("accepted channel cycle completed", token, DSD_APP_COMMAND_RESULT_COMPLETED);
    rc |= expect_int("accepted channel cycle skips empty entry", state.lcn_freq_roll, 2);
    rc |= expect_int("accepted channel cycle clears tuned", opts.p25_is_tuned, 0);
    rc |= expect_int("accepted channel cycle clears TG", state.lasttg, 0);
    rc |= expect_true("accepted channel cycle clears P25 VC", state.p25_vc_freq[0] == 0L);
    rc |= expect_true("accepted channel cycle refreshes CC sync", state.last_cc_sync_time_m > 42.0);
    rc |= expect_int("accepted channel cycle selects P25 SPS profile", state.sps_hunt_idx,
                     DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    rc |= expect_int("accepted channel cycle resets SPS hunt counter", state.sps_hunt_counter, 0);
    rc |= expect_int("accepted channel cycle TED SPS", g_cc_tune_ted_sps, 10);
    rc |= expect_int("accepted channel cycle profile staged before commit", g_cc_profile_at_tune,
                     DSD_FRAME_SYNC_SPS_PROFILE_6000_4);

    token = 0;
    rc |= expect_int("later channel cycle queued", dsd_app_command_action_tracked(DSD_APP_CMD_CHANNEL_CYCLE, &token),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("later channel cycle drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_command_status("later channel cycle completed", token, DSD_APP_COMMAND_RESULT_COMPLETED);
    rc |= expect_true("later channel cycle reaches frequency after empty entry", g_cc_tune_freq == 858000000L);
    rc |= expect_int("later channel cycle advances past second frequency", state.lcn_freq_roll, 4);

    token = 0;
    rc |= expect_int("wrapped channel cycle queued", dsd_app_command_action_tracked(DSD_APP_CMD_CHANNEL_CYCLE, &token),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("wrapped channel cycle drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_command_status("wrapped channel cycle completed", token, DSD_APP_COMMAND_RESULT_COMPLETED);
    rc |= expect_true("wrapped channel cycle skips leading empty entry", g_cc_tune_freq == 857000000L);
    rc |= expect_int("wrapped channel cycle advances from recovered entry", state.lcn_freq_roll, 2);
    freeState(&state);

    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    reset_cc_tune_stub(DSD_TRUNK_TUNE_RESULT_OK);
    return rc;
}
#endif

int
main(void) {
    int rc = 0;
    rc |= test_typed_command_api_wrappers();
    rc |= test_setter_coalescing_preserves_fifo_boundaries();
    rc |= test_tracked_command_results();
    rc |= test_visibility_and_queue_overflow();
    rc |= test_key_and_runtime_state_commands();
    rc |= test_file_network_and_import_commands();
    rc |= test_io_and_legacy_state_commands();
#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
    rc |= test_manual_tune_commands_commit_only_after_acceptance();
#endif
    if (rc == 0) {
        printf("DSD_APP_CMD_QUEUE: OK\n");
    }
    return rc;
}
