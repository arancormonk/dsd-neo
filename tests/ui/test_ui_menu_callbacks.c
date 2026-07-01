// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-branch-clone,misc-redundant-expression)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Deterministic contracts for terminal UI menu prompt callbacks.
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/ui/ui_cmd.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "command_dispatch.h"

#include "../../src/app_control/commands_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/ui/menu_core.h"
#include "menu_callbacks.h"
#include "menu_env.h"
#include "menu_internal.h"
#include "menu_prompts.h"

typedef struct {
    int id;
    size_t n;
    uint8_t data[DSD_APP_CMD_DISPATCH_DATA_MAX];
    int calls;
} CmdCapture;

typedef struct {
    char title[128];
    char prefill[256];
    size_t cap;
    void* user;
    ui_prompt_string_done_fn str_cb;
    ui_prompt_int_done_fn int_cb;
    int initial_int;
    int calls;
} PromptCapture;

static CmdCapture g_cmd;
static PromptCapture g_prompt;
static char g_status[256];
static int g_status_calls;
static char g_env_name[128];
static char g_env_value[128];
static int g_env_set_calls;
static int g_env_unset_calls;
static char g_env_int_name[128];
static int g_env_int_value;
static int g_env_int_calls;
static char g_env_double_name[128];
static double g_env_double_value;
static int g_env_double_calls;
static int g_reparse_calls;
static const char* g_env_get_value;
static int g_config_load_rc;
static int g_profile_load_rc;
static int g_config_save_rc;
static int g_snapshot_calls;
static dsd_app_config_metadata_payload g_config_metadata;
static int g_config_metadata_calls;

int
dsd_app_post_cmd(int cmd_id, const void* payload, size_t payload_sz) {
    g_cmd.id = cmd_id;
    g_cmd.n = payload_sz;
    if (payload_sz > sizeof(g_cmd.data)) {
        payload_sz = sizeof(g_cmd.data);
    }
    DSD_MEMSET(g_cmd.data, 0, sizeof(g_cmd.data));
    if (payload && payload_sz > 0) {
        DSD_MEMCPY(g_cmd.data, payload, payload_sz);
    }
    g_cmd.calls++;
    return 0;
}

int
dsd_app_command_action(int cmd_id) {
    return dsd_app_post_cmd(cmd_id, NULL, 0U);
}

int
dsd_app_command_set_i32(int cmd_id, int32_t value) {
    return dsd_app_post_cmd(cmd_id, &value, sizeof value);
}

int
dsd_app_command_set_u8(int cmd_id, uint8_t value) {
    return dsd_app_post_cmd(cmd_id, &value, sizeof value);
}

int
dsd_app_command_set_u32(int cmd_id, uint32_t value) {
    return dsd_app_post_cmd(cmd_id, &value, sizeof value);
}

int
dsd_app_command_set_u64(int cmd_id, uint64_t value) {
    return dsd_app_post_cmd(cmd_id, &value, sizeof value);
}

int
dsd_app_command_set_double(int cmd_id, double value) {
    return dsd_app_post_cmd(cmd_id, &value, sizeof value);
}

int
dsd_app_command_set_float(int cmd_id, float value) {
    return dsd_app_post_cmd(cmd_id, &value, sizeof value);
}

int
dsd_app_command_set_string(int cmd_id, const char* value) {
    return dsd_app_post_cmd(cmd_id, value, value ? strlen(value) + 1U : 0U);
}

int
dsd_app_command_set_endpoint(int cmd_id, const char* host, int32_t port) {
    dsd_app_endpoint_payload payload = {0};
    DSD_SNPRINTF(payload.host, sizeof payload.host, "%s", host ? host : "");
    payload.port = port;
    return dsd_app_post_cmd(cmd_id, &payload, sizeof payload);
}

int
dsd_app_command_set_udp_input(const char* bind, int32_t port) {
    dsd_app_udp_input_payload payload = {0};
    DSD_SNPRINTF(payload.bind, sizeof payload.bind, "%s", bind ? bind : "");
    payload.port = port;
    return dsd_app_post_cmd(DSD_APP_CMD_UDP_INPUT_CFG, &payload, sizeof payload);
}

int
dsd_app_command_set_p25_p2_params(const dsd_app_p25_p2_params_payload* payload) {
    return dsd_app_post_cmd(DSD_APP_CMD_P25_P2_PARAMS_SET, payload, payload ? sizeof *payload : 0U);
}

int
dsd_app_command_set_hytera_key(const dsd_app_hytera_key_payload* payload) {
    return dsd_app_post_cmd(DSD_APP_CMD_KEY_HYTERA_SET, payload, payload ? sizeof *payload : 0U);
}

int
dsd_app_command_set_aes_key(const dsd_app_aes_key_payload* payload) {
    return dsd_app_post_cmd(DSD_APP_CMD_KEY_AES_SET, payload, payload ? sizeof *payload : 0U);
}

int
dsd_app_command_dsp_op(const dsd_app_dsp_payload* payload) {
    return dsd_app_post_cmd(DSD_APP_CMD_DSP_OP, payload, payload ? sizeof *payload : 0U);
}

int
dsd_app_command_apply_config(const dsdneoUserConfig* config) {
    return dsd_app_post_cmd(DSD_APP_CMD_CONFIG_APPLY, config, config ? sizeof *config : 0U);
}

int
dsd_app_command_set_config_metadata(const dsd_app_config_metadata_payload* payload) {
    DSD_MEMSET(&g_config_metadata, 0, sizeof g_config_metadata);
    if (payload) {
        g_config_metadata = *payload;
    }
    g_config_metadata_calls++;
    return dsd_app_post_cmd(DSD_APP_CMD_CONFIG_METADATA_SET, payload, payload ? sizeof *payload : 0U);
}

void ui_statusf(const char* fmt, ...) DSD_ATTR_FORMAT(printf, 1, 2);

void
ui_statusf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    (void)DSD_VSNPRINTF(g_status, sizeof g_status, fmt, ap);
    va_end(ap);
    g_status_calls++;
}

void
ui_prompt_open_string_async_impl(const char* title, const char* prefill, size_t cap, void* user,
                                 ui_prompt_string_done_fn on_done) {
    DSD_SNPRINTF(g_prompt.title, sizeof g_prompt.title, "%s", title ? title : "");
    DSD_SNPRINTF(g_prompt.prefill, sizeof g_prompt.prefill, "%s", prefill ? prefill : "");
    g_prompt.cap = cap;
    g_prompt.user = user;
    g_prompt.str_cb = on_done;
    g_prompt.int_cb = NULL;
    g_prompt.calls++;
}

void
ui_prompt_open_int_async_impl(const char* title, int initial, void* user, ui_prompt_int_done_fn cb) {
    DSD_SNPRINTF(g_prompt.title, sizeof g_prompt.title, "%s", title ? title : "");
    g_prompt.initial_int = initial;
    g_prompt.user = user;
    g_prompt.int_cb = cb;
    g_prompt.str_cb = NULL;
    g_prompt.calls++;
}

void
ui_prompt_open_double_async_impl(const char* title, double initial, void* user, ui_prompt_double_done_fn cb) {
    (void)title;
    (void)initial;
    (void)user;
    (void)cb;
    assert(!"double prompt should not be opened by this test batch");
}

void
env_set_int(const char* name, int v) {
    DSD_SNPRINTF(g_env_int_name, sizeof g_env_int_name, "%s", name ? name : "");
    g_env_int_value = v;
    g_env_int_calls++;
}

void
env_set_double(const char* name, double v) {
    DSD_SNPRINTF(g_env_double_name, sizeof g_env_double_name, "%s", name ? name : "");
    g_env_double_value = v;
    g_env_double_calls++;
}

int
env_get_int(const char* name, int defv) {
    (void)name;
    return defv;
}

double
env_get_double(const char* name, double defv) {
    (void)name;
    return defv;
}

void
env_reparse_runtime_cfg(dsd_opts* opts) {
    (void)opts;
    g_reparse_calls++;
}

int
parse_hex_u64(const char* s, unsigned long long* out) {
    if (!s || !*s || !out) {
        return 0;
    }
    errno = 0;
    char* end = NULL;
    unsigned long long v = strtoull(s, &end, 16);
    if (errno != 0 || !end || *end != '\0') {
        return 0;
    }
    *out = v;
    return 1;
}

int
dsd_setenv(const char* name, const char* value, int overwrite) {
    DSD_SNPRINTF(g_env_name, sizeof g_env_name, "%s", name ? name : "");
    DSD_SNPRINTF(g_env_value, sizeof g_env_value, "%s", value ? value : "");
    g_env_set_calls++;
    (void)overwrite;
    return 0;
}

int
dsd_unsetenv(const char* name) {
    DSD_SNPRINTF(g_env_name, sizeof g_env_name, "%s", name ? name : "");
    g_env_unset_calls++;
    return 0;
}

const char*
dsd_neo_env_get(const char* name) {
    (void)name;
    return g_env_get_value;
}

int
dsd_user_config_load(const char* path, dsdneoUserConfig* cfg) {
    (void)path;
    if (cfg) {
        DSD_MEMSET(cfg, 0, sizeof *cfg);
    }
    return g_config_load_rc;
}

int
dsd_user_config_load_profile(const char* path, const char* profile_name, dsdneoUserConfig* cfg) {
    (void)path;
    (void)profile_name;
    if (cfg) {
        DSD_MEMSET(cfg, 0, sizeof *cfg);
    }
    return g_profile_load_rc;
}

int
dsd_user_config_save_atomic(const char* path, const dsdneoUserConfig* cfg) {
    (void)path;
    (void)cfg;
    return g_config_save_rc;
}

void
dsd_snapshot_opts_to_user_config(const dsd_opts* opts, const dsd_state* state, dsdneoUserConfig* cfg) {
    (void)opts;
    (void)state;
    if (cfg) {
        DSD_MEMSET(cfg, 0, sizeof *cfg);
    }
    g_snapshot_calls++;
}

static void
reset_capture(void) {
    DSD_MEMSET(&g_cmd, 0, sizeof g_cmd);
    DSD_MEMSET(&g_prompt, 0, sizeof g_prompt);
    DSD_MEMSET(g_status, 0, sizeof g_status);
    g_status_calls = 0;
    DSD_MEMSET(g_env_name, 0, sizeof g_env_name);
    DSD_MEMSET(g_env_value, 0, sizeof g_env_value);
    g_env_set_calls = 0;
    g_env_unset_calls = 0;
    DSD_MEMSET(g_env_int_name, 0, sizeof g_env_int_name);
    g_env_int_value = 0;
    g_env_int_calls = 0;
    DSD_MEMSET(g_env_double_name, 0, sizeof g_env_double_name);
    g_env_double_value = 0.0;
    g_env_double_calls = 0;
    g_reparse_calls = 0;
    g_env_get_value = NULL;
    g_config_load_rc = 0;
    g_profile_load_rc = 0;
    g_config_save_rc = 0;
    g_snapshot_calls = 0;
    DSD_MEMSET(&g_config_metadata, 0, sizeof g_config_metadata);
    g_config_metadata_calls = 0;
}

static UiCtx
make_ctx(dsd_opts* opts, dsd_state* state) {
    UiCtx ctx;
    ctx.opts = opts;
    ctx.state = state;
    return ctx;
}

static int
expect_str(const char* tag, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "%s: got '%s' want '%s'\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_cmd_string(const char* tag, int want_id, const char* want_payload) {
    int rc = 0;
    rc |= expect_int(tag, g_cmd.calls, 1);
    rc |= expect_int(tag, g_cmd.id, want_id);
    rc |= expect_str(tag, (const char*)g_cmd.data, want_payload);
    return rc;
}

static int32_t
cmd_i32(void) {
    int32_t v = 0;
    assert(g_cmd.n >= sizeof v);
    DSD_MEMCPY(&v, g_cmd.data, sizeof v);
    return v;
}

static uint32_t
cmd_u32(void) {
    uint32_t v = 0;
    assert(g_cmd.n >= sizeof v);
    DSD_MEMCPY(&v, g_cmd.data, sizeof v);
    return v;
}

static uint64_t
cmd_u64(void) {
    uint64_t v = 0;
    assert(g_cmd.n >= sizeof v);
    DSD_MEMCPY(&v, g_cmd.data, sizeof v);
    return v;
}

static double
cmd_double(void) {
    double v = 0.0;
    assert(g_cmd.n >= sizeof v);
    DSD_MEMCPY(&v, g_cmd.data, sizeof v);
    return v;
}

static char*
dup_test_string(const char* s) {
    size_t n = strlen(s) + 1U;
    char* out = (char*)malloc(n);
    assert(out != NULL);
    DSD_MEMCPY(out, s, n);
    return out;
}

static ProfileSelCtx*
make_profile_ctx(dsd_state* state, const char* path, const char* profile) {
    ProfileSelCtx* pctx = (ProfileSelCtx*)calloc(1, sizeof *pctx);
    assert(pctx != NULL);
    pctx->state = state;
    DSD_SNPRINTF(pctx->path, sizeof pctx->path, "%s", path);
    pctx->n = 1;
    pctx->labels = (const char**)calloc(1, sizeof(char*));
    pctx->names = (const char**)calloc(1, sizeof(char*));
    assert(pctx->labels != NULL && pctx->names != NULL);
    pctx->names[0] = dup_test_string(profile);
    pctx->labels[0] = pctx->names[0];
    return pctx;
}

static PulseSelCtx*
make_pulse_ctx(dsd_state* state, const char* first, const char* second) {
    (void)state;
    PulseSelCtx* pctx = (PulseSelCtx*)calloc(1, sizeof *pctx);
    assert(pctx != NULL);
    pctx->n = 2;
    pctx->labels = (const char**)calloc(2, sizeof(char*));
    pctx->names = (const char**)calloc(2, sizeof(char*));
    pctx->bufs = (char**)calloc(2, sizeof(char*));
    assert(pctx->labels != NULL && pctx->names != NULL && pctx->bufs != NULL);
    pctx->names[0] = dup_test_string(first);
    pctx->names[1] = dup_test_string(second);
    pctx->bufs[0] = dup_test_string("label-buffer-0");
    pctx->bufs[1] = dup_test_string("label-buffer-1");
    pctx->labels[0] = pctx->bufs[0];
    pctx->labels[1] = pctx->bufs[1];
    return pctx;
}

static int
test_path_and_file_callbacks(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    static UiCtx ctx;
    ctx = make_ctx(&opts, &state);

    reset_capture();
    cb_event_log_set(&ctx, "");
    rc |= expect_int("empty event path no command", g_cmd.calls, 0);

    reset_capture();
    cb_event_log_set(&ctx, "/tmp/events.log");
    rc |= expect_cmd_string("event path command", DSD_APP_CMD_EVENT_LOG_SET, "/tmp/events.log");

    reset_capture();
    cb_static_wav(&ctx, "static.wav");
    rc |= expect_cmd_string("static wav command", DSD_APP_CMD_WAV_STATIC_OPEN, "static.wav");

    reset_capture();
    cb_raw_wav(&ctx, "raw.wav");
    rc |= expect_cmd_string("raw wav command", DSD_APP_CMD_WAV_RAW_OPEN, "raw.wav");

    reset_capture();
    cb_dsp_out(&ctx, "capture-base");
    rc |= expect_cmd_string("dsp out command", DSD_APP_CMD_DSP_OUT_SET, "capture-base");

    reset_capture();
    cb_import_chan(&ctx, "channels.csv");
    rc |= expect_cmd_string("channel import command", DSD_APP_CMD_IMPORT_CHANNEL_MAP, "channels.csv");

    reset_capture();
    cb_import_group(&ctx, "groups.csv");
    rc |= expect_cmd_string("group import command", DSD_APP_CMD_IMPORT_GROUP_LIST, "groups.csv");

    reset_capture();
    cb_keys_dec(&ctx, "keys-dec.csv");
    rc |= expect_cmd_string("keys dec command", DSD_APP_CMD_IMPORT_KEYS_DEC, "keys-dec.csv");

    reset_capture();
    cb_keys_hex(&ctx, "keys-hex.csv");
    rc |= expect_cmd_string("keys hex command", DSD_APP_CMD_IMPORT_KEYS_HEX, "keys-hex.csv");

    reset_capture();
    cb_io_save_symbol_capture(&ctx, "symbols.bin");
    rc |= expect_cmd_string("symbol capture command", DSD_APP_CMD_SYMCAP_OPEN, "symbols.bin");

    reset_capture();
    cb_io_read_symbol_bin(&ctx, "replay.bin");
    rc |= expect_cmd_string("symbol input read command", DSD_APP_CMD_SYMBOL_IN_OPEN, "replay.bin");

    reset_capture();
    cb_switch_to_wav(&ctx, "input.wav");
    rc |= expect_cmd_string("wav input command", DSD_APP_CMD_INPUT_WAV_SET, "input.wav");

    reset_capture();
    cb_switch_to_symbol(&ctx, "capture.bin");
    rc |= expect_cmd_string("symbol bin route", DSD_APP_CMD_SYMBOL_IN_OPEN, "capture.bin");

    reset_capture();
    cb_switch_to_symbol(&ctx, "stream.raw");
    rc |= expect_cmd_string("symbol stream route", DSD_APP_CMD_INPUT_SYM_STREAM_SET, "stream.raw");

    return rc;
}

static int
test_config_callbacks_apply_and_report_failures(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    static UiCtx ctx;
    ctx = make_ctx(&opts, &state);

    reset_capture();
    cb_config_load(&ctx, "");
    rc |= expect_str("config load cancel status", g_status, "Config load canceled");
    rc |= expect_int("config load cancel no command", g_cmd.calls, 0);

    reset_capture();
    g_config_load_rc = -1;
    cb_config_load(&ctx, "/tmp/bad.toml");
    rc |= expect_str("config load failure", g_status, "Failed to load config from /tmp/bad.toml");
    rc |= expect_int("config load failure no command", g_cmd.calls, 0);

    reset_capture();
    cb_config_load(&ctx, "/tmp/good.toml");
    rc |= expect_int("config load command count", g_cmd.calls, 2);
    rc |= expect_int("config load command", g_cmd.id, DSD_APP_CMD_CONFIG_APPLY);
    rc |= expect_int("config load metadata count", g_config_metadata_calls, 1);
    rc |= expect_int("config load autosave enabled", g_config_metadata.autosave_enabled, 1);
    rc |= expect_str("config load autosave path", g_config_metadata.path, "/tmp/good.toml");
    rc |= expect_str("config load success status", g_status, "Config loaded from /tmp/good.toml");

    reset_capture();
    cb_config_save_as(&ctx, NULL);
    rc |= expect_str("config save cancel status", g_status, "Config save canceled");
    rc |= expect_int("config save cancel no snapshot", g_snapshot_calls, 0);

    reset_capture();
    cb_config_save_as(&ctx, "/tmp/save.toml");
    rc |= expect_int("config save snapshots", g_snapshot_calls, 1);
    rc |= expect_int("config save metadata command", g_cmd.id, DSD_APP_CMD_CONFIG_METADATA_SET);
    rc |= expect_int("config save metadata count", g_config_metadata_calls, 1);
    rc |= expect_int("config save autosave enabled", g_config_metadata.autosave_enabled, 1);
    rc |= expect_str("config save autosave path", g_config_metadata.path, "/tmp/save.toml");
    rc |= expect_str("config save success status", g_status, "Config saved to /tmp/save.toml");

    reset_capture();
    g_config_save_rc = -1;
    cb_config_save_as(&ctx, "/tmp/save-fail.toml");
    rc |= expect_int("config save failure snapshots", g_snapshot_calls, 1);
    rc |= expect_str("config save failure status", g_status, "Failed to save config to /tmp/save-fail.toml");

    reset_capture();
    ProfileSelCtx* pctx = make_profile_ctx(&state, "/tmp/profiles.toml", "mobile");
    chooser_done_config_profile(pctx, 0);
    rc |= expect_int("profile load command count", g_cmd.calls, 2);
    rc |= expect_int("profile load command", g_cmd.id, DSD_APP_CMD_CONFIG_APPLY);
    rc |= expect_int("profile load metadata count", g_config_metadata_calls, 1);
    rc |= expect_int("profile load disables autosave", g_config_metadata.autosave_enabled, 0);
    rc |= expect_str("profile load path", g_config_metadata.path, "/tmp/profiles.toml");
    rc |= expect_str("profile load status", g_status, "Profile loaded: mobile");

    reset_capture();
    pctx = make_profile_ctx(&state, "/tmp/profiles.toml", "field");
    g_profile_load_rc = -1;
    chooser_done_config_profile(pctx, 0);
    rc |= expect_int("profile failure no command", g_cmd.calls, 0);
    rc |= expect_str("profile load failure", g_status, "Failed to load profile field from /tmp/profiles.toml");

    reset_capture();
    pctx = make_profile_ctx(&state, "/tmp/profiles.toml", "base");
    chooser_done_config_profile(pctx, -1);
    rc |= expect_int("profile cancel no command", g_cmd.calls, 0);
    rc |= expect_int("profile cancel no status", g_status_calls, 0);

    return rc;
}

static int
test_typed_callbacks_clamp_and_cancel(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    static UiCtx ctx;
    ctx = make_ctx(&opts, &state);

    reset_capture();
    cb_setmod_bw(&ctx, 0, 1234);
    rc |= expect_int("canceled setmod no command", g_cmd.calls, 0);

    reset_capture();
    cb_setmod_bw(&ctx, 1, 40000);
    rc |= expect_int("setmod command", g_cmd.id, DSD_APP_CMD_RIGCTL_SET_MOD_BW);
    rc |= expect_int("setmod clamped", cmd_i32(), 25000);
    rc |= expect_int("setmod adjusted status", strstr(g_status, "adjusted") != NULL, 1);

    reset_capture();
    cb_tg_hold(&ctx, 1, -7);
    rc |= expect_int("tg hold command", g_cmd.id, DSD_APP_CMD_TG_HOLD_SET);
    rc |= expect_int("tg hold clamped", (int)cmd_u32(), 0);

    reset_capture();
    cb_hangtime(&ctx, 1, -1.25);
    rc |= expect_int("hangtime command", g_cmd.id, DSD_APP_CMD_HANGTIME_SET);
    rc |= expect_int("hangtime clamped", cmd_double() == 0.0, 1);

    reset_capture();
    cb_slot_pref(&ctx, 1, 7);
    rc |= expect_int("slot pref command", g_cmd.id, DSD_APP_CMD_SLOT_PREF_SET);
    rc |= expect_int("slot pref clamped zero-based", cmd_i32(), 1);

    reset_capture();
    cb_slots_on(&ctx, 1, -9);
    rc |= expect_int("slot mask command", g_cmd.id, DSD_APP_CMD_SLOTS_ONOFF_SET);
    rc |= expect_int("slot mask clamped", cmd_i32(), 0);

    return rc;
}

static int
test_key_callbacks_validate_and_pack(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    static UiCtx ctx;
    ctx = make_ctx(&opts, &state);
    static const char tyt_ap_value[] = "fixture-tyt-ap";
    static const char retevis_rc2_value[] = "fixture-retevis-rc2";
    static const char tyt_ep_value[] = "fixture-tyt-ep";

    reset_capture();
    cb_tyt_ap(&ctx, "");
    rc |= expect_int("empty tyt ap no command", g_cmd.calls, 0);

    reset_capture();
    cb_tyt_ap(&ctx, tyt_ap_value);
    rc |= expect_cmd_string("tyt ap command", DSD_APP_CMD_KEY_TYT_AP_SET, tyt_ap_value);

    reset_capture();
    cb_retevis_rc2(&ctx, retevis_rc2_value);
    rc |= expect_cmd_string("retevis rc2 command", DSD_APP_CMD_KEY_RETEVIS_RC2_SET, retevis_rc2_value);

    reset_capture();
    cb_tyt_ep(&ctx, tyt_ep_value);
    rc |= expect_cmd_string("tyt ep command", DSD_APP_CMD_KEY_TYT_EP_SET, tyt_ep_value);

    reset_capture();
    cb_ken_scr(&ctx, "123456789");
    rc |= expect_cmd_string("kenwood scrambler command", DSD_APP_CMD_KEY_KEN_SCR_SET, "123456789");

    reset_capture();
    cb_anytone_bp(&ctx, "1A2B");
    rc |= expect_cmd_string("anytone bp command", DSD_APP_CMD_KEY_ANYTONE_BP_SET, "1A2B");

    reset_capture();
    cb_xor_ks(&ctx, "4:01020304");
    rc |= expect_cmd_string("xor keystream command", DSD_APP_CMD_KEY_XOR_SET, "4:01020304");

    reset_capture();
    cb_key_basic(&ctx, 1, 999);
    rc |= expect_int("basic key command", g_cmd.id, DSD_APP_CMD_KEY_BASIC_SET);
    rc |= expect_int("basic key clamp", (int)cmd_u32(), 255);

    reset_capture();
    cb_key_scrambler(&ctx, 1, 999999);
    rc |= expect_int("scrambler key command", g_cmd.id, DSD_APP_CMD_KEY_SCRAMBLER_SET);
    rc |= expect_int("scrambler key clamp", (int)cmd_u32(), 0x7FFF);

    reset_capture();
    cb_key_rc4des(&ctx, "not-hex");
    rc |= expect_int("invalid rc4des no command", g_cmd.calls, 0);

    reset_capture();
    cb_key_rc4des(&ctx, "1A2B");
    rc |= expect_int("rc4des command", g_cmd.id, DSD_APP_CMD_KEY_RC4DES_SET);
    rc |= expect_int("rc4des value", cmd_u64() == 0x1A2BULL, 1);

    reset_capture();
    HyCtx* hc = (HyCtx*)calloc(1, sizeof *hc);
    assert(hc != NULL);
    hc->c = &ctx;
    cb_hytera_step(hc, "not-hex");
    rc |= expect_int("invalid hytera reprompts", g_prompt.calls, 1);
    rc |= expect_int("invalid hytera no command", g_cmd.calls, 0);
    cb_hytera_step(hc, "10");
    cb_hytera_step(hc, "20");
    cb_hytera_step(hc, "30");
    cb_hytera_step(hc, "40");
    rc |= expect_int("hytera command", g_cmd.id, DSD_APP_CMD_KEY_HYTERA_SET);
    uint64_t hy[5] = {0};
    DSD_MEMCPY(hy, g_cmd.data, sizeof hy);
    rc |= expect_int("hytera H", hy[0] == 0x10ULL, 1);
    rc |= expect_int("hytera K1", hy[1] == 0x10ULL, 1);
    rc |= expect_int("hytera K4", hy[4] == 0x40ULL, 1);

    reset_capture();
    AesCtx* ac = (AesCtx*)calloc(1, sizeof *ac);
    assert(ac != NULL);
    ac->c = &ctx;
    cb_aes_step(ac, "bad-hex");
    rc |= expect_int("invalid aes reprompts", g_prompt.calls, 1);
    rc |= expect_str("invalid aes keeps bad prefill", g_prompt.prefill, "bad-hex");
    rc |= expect_int("invalid aes no command", g_cmd.calls, 0);
    cb_aes_step(ac, "1");
    cb_aes_step(ac, "2");
    cb_aes_step(ac, "3");
    cb_aes_step(ac, "4");
    rc |= expect_int("aes command", g_cmd.id, DSD_APP_CMD_KEY_AES_SET);
    uint64_t aes[4] = {0};
    DSD_MEMCPY(aes, g_cmd.data, sizeof aes);
    rc |= expect_int("aes K1", aes[0] == 1ULL, 1);
    rc |= expect_int("aes K4", aes[3] == 4ULL, 1);

    reset_capture();
    state.p2_sysid = 0x2AAU;
    state.p2_cc = 0x3BBU;
    P2Ctx* pc = (P2Ctx*)calloc(1, sizeof *pc);
    assert(pc != NULL);
    pc->c = &ctx;
    cb_p2_step(pc, "12345");
    rc |= expect_int("p2 sysid prompt", g_prompt.calls, 1);
    rc |= expect_str("p2 sysid prefill", g_prompt.prefill, "2AA");
    cb_p2_step(pc, "678");
    rc |= expect_str("p2 cc prefill", g_prompt.prefill, "3BB");
    cb_p2_step(pc, "9AB");
    rc |= expect_int("p2 params command", g_cmd.id, DSD_APP_CMD_P25_P2_PARAMS_SET);
    uint64_t p2[3] = {0};
    DSD_MEMCPY(p2, g_cmd.data, sizeof p2);
    rc |= expect_int("p2 wacn", p2[0] == 0x12345ULL, 1);
    rc |= expect_int("p2 sysid", p2[1] == 0x678ULL, 1);
    rc |= expect_int("p2 cc", p2[2] == 0x9ABULL, 1);

    return rc;
}

static int
test_network_and_io_callbacks_pack_payloads(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    opts.udp_portno = 4567;
    opts.tcp_portno = 3456;
    static UiCtx ctx;
    ctx = make_ctx(&opts, &state);

    reset_capture();
    UdpOutCtx* uo = (UdpOutCtx*)calloc(1, sizeof *uo);
    assert(uo != NULL);
    uo->c = &ctx;
    cb_udp_out_host(uo, "127.0.0.1");
    rc |= expect_int("udp out prompts for port", g_prompt.calls, 1);
    rc |= expect_int("udp out default port", g_prompt.initial_int, 4567);
    assert(g_prompt.int_cb != NULL);
    g_prompt.int_cb(g_prompt.user, 1, 9999);
    rc |= expect_int("udp out command", g_cmd.id, DSD_APP_CMD_UDP_OUT_CFG);

    struct {
        char host[256];
        int32_t port;
    } hp;

    DSD_MEMCPY(&hp, g_cmd.data, sizeof hp);
    rc |= expect_str("udp out host", hp.host, "127.0.0.1");
    rc |= expect_int("udp out port", hp.port, 9999);

    reset_capture();
    TcpLinkCtx* tc = (TcpLinkCtx*)calloc(1, sizeof *tc);
    assert(tc != NULL);
    tc->c = &ctx;
    cb_tcp_host(tc, "radio.local");
    rc |= expect_int("tcp prompts for port", g_prompt.calls, 1);
    rc |= expect_int("tcp default port", g_prompt.initial_int, 3456);
    assert(g_prompt.int_cb != NULL);
    g_prompt.int_cb(g_prompt.user, 1, 7355);
    rc |= expect_int("tcp command", g_cmd.id, DSD_APP_CMD_TCP_CONNECT_AUDIO_CFG);
    DSD_MEMCPY(&hp, g_cmd.data, sizeof hp);
    rc |= expect_str("tcp host", hp.host, "radio.local");
    rc |= expect_int("tcp port", hp.port, 7355);

    reset_capture();
    opts.udp_in_portno = 2468;
    UdpInCtx* ui = (UdpInCtx*)calloc(1, sizeof *ui);
    assert(ui != NULL);
    ui->c = &ctx;
    cb_udp_in_addr(ui, "0.0.0.0");
    rc |= expect_int("udp in prompts for port", g_prompt.calls, 1);
    rc |= expect_int("udp in default port", g_prompt.initial_int, 2468);
    assert(g_prompt.int_cb != NULL);
    g_prompt.int_cb(g_prompt.user, 1, 8899);
    rc |= expect_int("udp in command", g_cmd.id, DSD_APP_CMD_UDP_INPUT_CFG);
    DSD_MEMCPY(&hp, g_cmd.data, sizeof hp);
    rc |= expect_str("udp in bind", hp.host, "0.0.0.0");
    rc |= expect_int("udp in port", hp.port, 8899);

    reset_capture();
    opts.rigctlportno = 4444;
    RigCtx* rig_ok = (RigCtx*)calloc(1, sizeof *rig_ok);
    assert(rig_ok != NULL);
    rig_ok->c = &ctx;
    cb_rig_host(rig_ok, "rig.local");
    rc |= expect_int("rig prompts for port", g_prompt.calls, 1);
    rc |= expect_int("rig default port", g_prompt.initial_int, 4444);
    assert(g_prompt.int_cb != NULL);
    g_prompt.int_cb(g_prompt.user, 1, 4555);
    rc |= expect_int("rig command", g_cmd.id, DSD_APP_CMD_RIGCTL_CONNECT_CFG);
    DSD_MEMCPY(&hp, g_cmd.data, sizeof hp);
    rc |= expect_str("rig host", hp.host, "rig.local");
    rc |= expect_int("rig port", hp.port, 4555);

    reset_capture();
    RigCtx* rig = (RigCtx*)calloc(1, sizeof *rig);
    assert(rig != NULL);
    rig->c = &ctx;
    cb_rig_host(rig, "");
    rc |= expect_int("empty rig host frees without prompt", g_prompt.calls, 0);
    rc |= expect_int("empty rig host no command", g_cmd.calls, 0);

    return rc;
}

static int
test_gain_rtl_and_env_callbacks(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    static UiCtx ctx;
    ctx = make_ctx(&opts, &state);

    reset_capture();
    cb_gain_dig(&ctx, 1, 70.0);
    rc |= expect_int("digital gain command", g_cmd.id, DSD_APP_CMD_GAIN_SET);
    rc |= expect_int("digital gain clamped", cmd_i32(), 50);

    reset_capture();
    cb_gain_ana(&ctx, 1, -3.0);
    rc |= expect_int("analog gain command", g_cmd.id, DSD_APP_CMD_AGAIN_SET);
    rc |= expect_int("analog gain lower clamp", cmd_i32(), 0);
    rc |= expect_int("analog gain adjusted status", strstr(g_status, "adjusted") != NULL, 1);

    reset_capture();
    cb_input_vol(&ctx, 1, 99);
    rc |= expect_int("input volume command", g_cmd.id, DSD_APP_CMD_INPUT_VOL_SET);
    rc |= expect_int("input volume upper clamp", cmd_i32(), 16);

    reset_capture();
    cb_rtl_dev(&ctx, 1, 2);
    rc |= expect_int("rtl dev command", g_cmd.id, DSD_APP_CMD_RTL_SET_DEV);
    rc |= expect_int("rtl dev value", cmd_i32(), 2);

    reset_capture();
    cb_rtl_freq(&ctx, 1, 851012500);
    rc |= expect_int("rtl freq command", g_cmd.id, DSD_APP_CMD_RTL_SET_FREQ);
    rc |= expect_int("rtl freq value", cmd_i32(), 851012500);

    reset_capture();
    cb_rtl_gain(&ctx, 1, 77);
    rc |= expect_int("rtl gain command", g_cmd.id, DSD_APP_CMD_RTL_SET_GAIN);
    rc |= expect_int("rtl gain clamped", cmd_i32(), 49);

    reset_capture();
    cb_rtl_ppm(&ctx, 1, -250);
    rc |= expect_int("rtl ppm command", g_cmd.id, DSD_APP_CMD_RTL_SET_PPM);
    rc |= expect_int("rtl ppm clamped", cmd_i32(), -200);

    reset_capture();
    cb_rtl_bw(&ctx, 1, 13);
    rc |= expect_int("rtl bw command", g_cmd.id, DSD_APP_CMD_RTL_SET_BW);
    rc |= expect_int("rtl bw nearest", cmd_i32(), 12);

    reset_capture();
    cb_rtl_sql(&ctx, 1, -42.5);
    rc |= expect_int("rtl sql command", g_cmd.id, DSD_APP_CMD_RTL_SET_SQL_DB);
    rc |= expect_int("rtl sql value", cmd_double() == -42.5, 1);

    reset_capture();
    cb_rtl_vol(&ctx, 1, 9);
    rc |= expect_int("rtl vol command", g_cmd.id, DSD_APP_CMD_RTL_SET_VOL_MULT);
    rc |= expect_int("rtl vol clamped", cmd_i32(), 3);

    reset_capture();
    cb_input_warn(&ctx, 1, 5.0);
    rc |= expect_int("input warn command", g_cmd.id, DSD_APP_CMD_INPUT_WARN_DB_SET);
    rc |= expect_int("input warn clamp", cmd_double() == 0.0, 1);
    rc |= expect_str("input warn env name", g_env_double_name, "DSD_NEO_INPUT_WARN_DB");
    rc |= expect_int("input warn env value", g_env_double_value == 0.0, 1);

    reset_capture();
    cb_audio_lpf(&ctx, 1, 0);
    rc |= expect_str("audio lpf off env name", g_env_name, "DSD_NEO_AUDIO_LPF");
    rc |= expect_str("audio lpf off env value", g_env_value, "off");
    rc |= expect_int("audio lpf off reparse", g_reparse_calls, 1);

    reset_capture();
    cb_auto_ppm_snr(&ctx, 1, 27.5);
    rc |= expect_str("auto ppm snr env name", g_env_double_name, "DSD_NEO_AUTO_PPM_SNR_DB");
    rc |= expect_int("auto ppm snr env value", g_env_double_value == 27.5, 1);

    reset_capture();
    cb_auto_ppm_pwr(&ctx, 1, -31.25);
    rc |= expect_str("auto ppm pwr env name", g_env_double_name, "DSD_NEO_AUTO_PPM_PWR_DB");
    rc |= expect_int("auto ppm pwr env value", g_env_double_value == -31.25, 1);

    reset_capture();
    cb_auto_ppm_zeroppm(&ctx, 1, 0.75);
    rc |= expect_str("auto ppm zero ppm env name", g_env_double_name, "DSD_NEO_AUTO_PPM_ZEROLOCK_PPM");
    rc |= expect_int("auto ppm zero ppm env value", g_env_double_value == 0.75, 1);

    reset_capture();
    cb_auto_ppm_zerohz(&ctx, 1, 125);
    rc |= expect_str("auto ppm zero hz env name", g_env_int_name, "DSD_NEO_AUTO_PPM_ZEROLOCK_HZ");
    rc |= expect_int("auto ppm zero hz env value", g_env_int_value, 125);

    reset_capture();
    P25NumCtx* pc = (P25NumCtx*)calloc(1, sizeof *pc);
    assert(pc != NULL);
    pc->name = "DSD_NEO_P25_TEST_NUM";
    cb_set_p25_num(pc, 1, 42.25);
    rc |= expect_str("p25 num env name", g_env_double_name, "DSD_NEO_P25_TEST_NUM");
    rc |= expect_int("p25 num env value", g_env_double_value == 42.25, 1);

    reset_capture();
    opts.audio_in_type = AUDIO_IN_RTL;
    cb_tcp_prebuf(&ctx, 1, 250);
    rc |= expect_str("tcp prebuf env name", g_env_int_name, "DSD_NEO_TCP_PREBUF_MS");
    rc |= expect_int("tcp prebuf env value", g_env_int_value, 250);
    rc |= expect_int("tcp prebuf restarts rtl", g_cmd.id, DSD_APP_CMD_RTL_RESTART);

    reset_capture();
    opts.audio_in_type = AUDIO_IN_RTL;
    cb_tcp_rcvbuf(&ctx, 1, 0);
    rc |= expect_str("tcp rcvbuf clear env name", g_env_name, "DSD_NEO_TCP_RCVBUF");
    rc |= expect_str("tcp rcvbuf clear env value", g_env_value, "");
    rc |= expect_int("tcp rcvbuf clear restarts rtl", g_cmd.id, DSD_APP_CMD_RTL_RESTART);

    reset_capture();
    opts.audio_in_type = AUDIO_IN_PULSE;
    cb_tcp_rcvtimeo(&ctx, 1, 1500);
    rc |= expect_str("tcp rcvtimeo env name", g_env_int_name, "DSD_NEO_TCP_RCVTIMEO");
    rc |= expect_int("tcp rcvtimeo env value", g_env_int_value, 1500);
    rc |= expect_int("tcp rcvtimeo pulse no restart", g_cmd.calls, 0);

    return rc;
}

static int
test_env_editor_and_protocol_callbacks(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    static UiCtx ctx;
    ctx = make_ctx(&opts, &state);

    reset_capture();
    EnvEditCtx* bad = (EnvEditCtx*)calloc(1, sizeof *bad);
    assert(bad != NULL);
    bad->c = &ctx;
    cb_env_edit_name(bad, "PATH");
    rc |= expect_int("invalid env name no prompt", g_prompt.calls, 0);
    rc |= expect_int("invalid env name status", strstr(g_status, "DSD_NEO_") != NULL, 1);

    reset_capture();
    g_env_get_value = "old";
    EnvEditCtx* ec = (EnvEditCtx*)calloc(1, sizeof *ec);
    assert(ec != NULL);
    ec->c = &ctx;
    cb_env_edit_name(ec, "DSD_NEO_TEST_VALUE");
    rc |= expect_int("valid env name prompts", g_prompt.calls, 1);
    rc |= expect_str("env prompt prefill", g_prompt.prefill, "old");
    assert(g_prompt.str_cb != NULL);
    g_prompt.str_cb(g_prompt.user, "new");
    rc |= expect_str("env set name", g_env_name, "DSD_NEO_TEST_VALUE");
    rc |= expect_str("env set value", g_env_value, "new");
    rc |= expect_int("env set reparse", g_reparse_calls, 1);

    reset_capture();
    EnvEditCtx* clear = (EnvEditCtx*)calloc(1, sizeof *clear);
    assert(clear != NULL);
    clear->c = &ctx;
    DSD_SNPRINTF(clear->name, sizeof clear->name, "%s", "DSD_NEO_CLEAR_ME");
    cb_env_edit_value(clear, "");
    rc |= expect_str("env clear name", g_env_name, "DSD_NEO_CLEAR_ME");
    rc |= expect_int("env clear calls unset", g_env_unset_calls, 1);
    rc |= expect_int("env clear reparses", g_reparse_calls, 1);

    reset_capture();
    cb_lr_custom(&ctx, "lrrp.txt");
    rc |= expect_cmd_string("lrrp custom", DSD_APP_CMD_LRRP_SET_CUSTOM, "lrrp.txt");

    reset_capture();
    M17Ctx* mc = (M17Ctx*)calloc(1, sizeof *mc);
    assert(mc != NULL);
    mc->c = &ctx;
    cb_m17_user_data(mc, "0,DEST,SOURCE");
    rc |= expect_cmd_string("m17 user data", DSD_APP_CMD_M17_USER_DATA_SET, "0,DEST,SOURCE");

    return rc;
}

static int
test_pulse_chooser_callbacks_pack_selected_names(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);

    reset_capture();
    PulseSelCtx* out = make_pulse_ctx(&state, "speaker.monitor", "headphones");
    chooser_done_pulse_out(out, 1);
    rc |= expect_cmd_string("pulse out selected command", DSD_APP_CMD_PULSE_OUT_SET, "headphones");
    rc |= expect_int("pulse out status", strstr(g_status, "Pulse out requested: headphones") != NULL, 1);

    reset_capture();
    PulseSelCtx* in = make_pulse_ctx(&state, "mic0", "line-in");
    chooser_done_pulse_in(in, 0);
    rc |= expect_cmd_string("pulse in selected command", DSD_APP_CMD_PULSE_IN_SET, "mic0");
    rc |= expect_int("pulse in status", strstr(g_status, "Pulse in requested: mic0") != NULL, 1);

    reset_capture();
    PulseSelCtx* cancel = make_pulse_ctx(&state, "unused0", "unused1");
    chooser_done_pulse_in(cancel, -1);
    rc |= expect_int("pulse cancel no command", g_cmd.calls, 0);
    rc |= expect_int("pulse cancel no status", g_status_calls, 0);

    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_path_and_file_callbacks();
    rc |= test_config_callbacks_apply_and_report_failures();
    rc |= test_typed_callbacks_clamp_and_cancel();
    rc |= test_key_callbacks_validate_and_pack();
    rc |= test_network_and_io_callbacks_pack_payloads();
    rc |= test_gain_rtl_and_env_callbacks();
    rc |= test_env_editor_and_protocol_callbacks();
    rc |= test_pulse_chooser_callbacks_pack_selected_names();
    if (rc == 0) {
        printf("UI_MENU_CALLBACKS: OK\n");
    }
    return rc;
}

// NOLINTEND(bugprone-branch-clone,misc-redundant-expression)
