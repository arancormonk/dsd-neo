// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Deterministic contracts for terminal UI menu action handlers.
 */

#include <assert.h>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/frontend.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/audio.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/call_alert.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <math.h>
#include <sndfile.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "command_dispatch.h"

#include "csv_picker.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/ui/menu_core.h"
#include "menu_actions.h"
#include "menu_callbacks.h"
#include "menu_env.h"
#include "menu_internal.h"
#include "menu_prompts.h"
#include "rr_panel.h"

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
    int initial_int;
    double initial_double;
    void* user;
    ui_prompt_string_done_fn str_cb;
    ui_prompt_int_done_fn int_cb;
    ui_prompt_double_done_fn double_cb;
    int calls;
} PromptCapture;

typedef struct {
    char title[128];
    const char* labels[32];
    int n;
    int initial_sel;
    void* user;
    void (*on_done)(void*, int);
    int calls;
} ChooserCapture;

static CmdCapture g_cmd;
static PromptCapture g_prompt;
static ChooserCapture g_chooser;
static char g_status[256];
static int g_status_calls;
static dsdneoRuntimeConfig g_cfg;
static int g_cfg_valid = 1;
static const char* g_default_config_path = "/tmp/default.toml";
static int g_save_atomic_rc;
static int g_snapshot_calls;
static int g_list_profiles_rc;
static char g_env_name[128];
static char g_env_value[128];
static int g_env_set_calls;
static int g_env_int_value = 77;
static double g_env_double_value = 12.5;
static int g_reparse_calls;
static int g_audio_enum_rc;
static dsd_audio_device g_audio_inputs[2];
static dsd_audio_device g_audio_outputs[2];
static dsd_app_config_metadata_payload g_config_metadata;
static int g_config_metadata_calls;

static void
reset_capture(void) {
    DSD_MEMSET(&g_cmd, 0, sizeof g_cmd);
    DSD_MEMSET(&g_prompt, 0, sizeof g_prompt);
    DSD_MEMSET(&g_chooser, 0, sizeof g_chooser);
    DSD_MEMSET(g_status, 0, sizeof g_status);
    g_status_calls = 0;
    DSD_MEMSET(&g_cfg, 0, sizeof g_cfg);
    g_cfg_valid = 1;
    g_default_config_path = "/tmp/default.toml";
    g_save_atomic_rc = 0;
    g_snapshot_calls = 0;
    g_list_profiles_rc = 0;
    DSD_MEMSET(g_env_name, 0, sizeof g_env_name);
    DSD_MEMSET(g_env_value, 0, sizeof g_env_value);
    g_env_set_calls = 0;
    g_env_int_value = 77;
    g_env_double_value = 12.5;
    g_reparse_calls = 0;
    g_audio_enum_rc = 0;
    DSD_MEMSET(g_audio_inputs, 0, sizeof g_audio_inputs);
    DSD_MEMSET(g_audio_outputs, 0, sizeof g_audio_outputs);
    DSD_MEMSET(&g_config_metadata, 0, sizeof g_config_metadata);
    g_config_metadata_calls = 0;
}

static void
release_prompt_user(void) {
    free(g_prompt.user);
    g_prompt.user = NULL;
}

static UiCtx
make_ctx(dsd_opts* opts, dsd_state* state) {
    UiCtx ctx;
    ctx.opts = opts;
    ctx.state = state;
    return ctx;
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
expect_str(const char* tag, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "%s: got '%s' want '%s'\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int32_t
cmd_i32(void) {
    int32_t v = 0;
    assert(g_cmd.n >= sizeof v);
    DSD_MEMCPY(&v, g_cmd.data, sizeof v);
    return v;
}

static void
free_profile_ctx(ProfileSelCtx* pctx) {
    if (!pctx) {
        return;
    }
    if (pctx->names) {
        for (int i = 0; i < pctx->n; i++) {
            free((void*)pctx->names[i]);
        }
    }
    free((void*)pctx->labels);
    free((void*)pctx->names);
    free(pctx);
}

static void
free_pulse_ctx(PulseSelCtx* pctx) {
    if (!pctx) {
        return;
    }
    if (pctx->names) {
        for (int i = 0; i < pctx->n; i++) {
            free((void*)pctx->names[i]);
        }
    }
    if (pctx->bufs) {
        for (int i = 0; i < pctx->n; i++) {
            free(pctx->bufs[i]);
        }
    }
    free((void*)pctx->labels);
    free((void*)pctx->names);
    free((void*)pctx->bufs);
    free(pctx);
}

static int
capture_command(int cmd_id, const void* payload, size_t payload_sz) {
    g_cmd.id = cmd_id;
    g_cmd.n = payload_sz;
    if (payload_sz > sizeof(g_cmd.data)) {
        payload_sz = sizeof(g_cmd.data);
    }
    DSD_MEMSET(g_cmd.data, 0, sizeof g_cmd.data);
    if (payload && payload_sz > 0) {
        DSD_MEMCPY(g_cmd.data, payload, payload_sz);
    }
    g_cmd.calls++;
    return 0;
}

double
pwr_to_dB(double mean_power) { // NOLINT(misc-use-internal-linkage)
    if (mean_power <= 0.0) {
        return -120.0;
    }
    return 10.0 * log10(mean_power);
}

int
dsd_app_frontend_get_metrics(dsd_frontend_metrics* out) { // NOLINT(misc-use-internal-linkage)
    if (!out) {
        return -1;
    }
    DSD_MEMSET(out, 0, sizeof *out);
    return 0;
}

int
dsd_app_command_action(int cmd_id) {
    return capture_command(cmd_id, NULL, 0U);
}

int
dsd_app_command_set_i32(int cmd_id, int32_t value) {
    return capture_command(cmd_id, &value, sizeof value);
}

int
dsd_app_command_set_u8(int cmd_id, uint8_t value) {
    return capture_command(cmd_id, &value, sizeof value);
}

int
dsd_app_command_set_u32(int cmd_id, uint32_t value) {
    return capture_command(cmd_id, &value, sizeof value);
}

int
dsd_app_command_set_u64(int cmd_id, uint64_t value) {
    return capture_command(cmd_id, &value, sizeof value);
}

int
dsd_app_command_set_double(int cmd_id, double value) {
    return capture_command(cmd_id, &value, sizeof value);
}

int
dsd_app_command_set_float(int cmd_id, float value) {
    return capture_command(cmd_id, &value, sizeof value);
}

int
dsd_app_command_set_string(int cmd_id, const char* value) {
    return capture_command(cmd_id, value, value ? strlen(value) + 1U : 0U);
}

int
dsd_app_command_set_endpoint(int cmd_id, const char* host, int32_t port) {
    dsd_app_endpoint_payload payload = {0};
    DSD_SNPRINTF(payload.host, sizeof payload.host, "%s", host ? host : "");
    payload.port = port;
    return capture_command(cmd_id, &payload, sizeof payload);
}

int
dsd_app_command_set_p25_p2_params(const dsd_app_p25_p2_params_payload* payload) {
    return capture_command(DSD_APP_CMD_P25_P2_PARAMS_SET, payload, payload ? sizeof *payload : 0U);
}

int
dsd_app_command_set_hytera_key(const dsd_app_hytera_key_payload* payload) {
    return capture_command(DSD_APP_CMD_KEY_HYTERA_SET, payload, payload ? sizeof *payload : 0U);
}

int
dsd_app_command_set_aes_key(const dsd_app_aes_key_payload* payload) {
    return capture_command(DSD_APP_CMD_KEY_AES_SET, payload, payload ? sizeof *payload : 0U);
}

int
dsd_app_command_dsp_op(const dsd_app_dsp_payload* payload) {
    return capture_command(DSD_APP_CMD_DSP_OP, payload, payload ? sizeof *payload : 0U);
}

int
dsd_app_command_apply_config(const dsdneoUserConfig* config) {
    return capture_command(DSD_APP_CMD_CONFIG_APPLY, config, config ? sizeof *config : 0U);
}

int
dsd_app_command_set_config_metadata(const dsd_app_config_metadata_payload* payload) {
    DSD_MEMSET(&g_config_metadata, 0, sizeof g_config_metadata);
    if (payload) {
        g_config_metadata = *payload;
    }
    g_config_metadata_calls++;
    return capture_command(DSD_APP_CMD_CONFIG_METADATA_SET, payload, payload ? sizeof *payload : 0U);
}

/* The picker opens on the mode in effect, so the readback is stubbed too. */
static dsdneoUserDecodeMode g_infer_mode = DSDCFG_MODE_AUTO;

dsdneoUserDecodeMode
dsd_infer_decode_mode_preset(const dsd_opts* opts) {
    (void)opts;
    return g_infer_mode;
}

/* The decoder-mode picker names its rows through the runtime; a per-mode string
   lets the test see which preset landed on which row. */
const char*
dsd_decode_mode_display_name(dsdneoUserDecodeMode mode) {
    static const char* const names[] = {"m0", "m1", "m2",  "m3",  "m4",  "m5",  "m6",  "m7",
                                        "m8", "m9", "m10", "m11", "m12", "m13", "m14", "m15"};
    const int i = (int)mode;
    return (i >= 0 && i < (int)(sizeof names / sizeof names[0])) ? names[i] : "m?";
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
ui_prompt_open_string_async(const char* title, const char* prefill, size_t cap, ui_prompt_string_done_fn on_done,
                            void* user) {
    DSD_SNPRINTF(g_prompt.title, sizeof g_prompt.title, "%s", title ? title : "");
    DSD_SNPRINTF(g_prompt.prefill, sizeof g_prompt.prefill, "%s", prefill ? prefill : "");
    g_prompt.cap = cap;
    g_prompt.user = user;
    g_prompt.str_cb = on_done;
    g_prompt.int_cb = NULL;
    g_prompt.double_cb = NULL;
    g_prompt.calls++;
}

/* menu_actions.c routes the import items through the CSV picker; with no imports
   directory of matching files it prompts, so forward to the prompt stub to keep
   the existing "opens the path prompt" assertions honest. */
void
ui_csv_import_picker_open(const char* kind, const char* prompt_title, size_t cap, ui_prompt_string_done_fn on_done,
                          void* user_ctx) {
    (void)kind;
    ui_prompt_open_string_async(prompt_title, NULL, cap, on_done, user_ctx);
}

void
ui_prompt_open_int_async(const char* title, int initial, ui_prompt_int_done_fn cb, void* user) {
    DSD_SNPRINTF(g_prompt.title, sizeof g_prompt.title, "%s", title ? title : "");
    g_prompt.initial_int = initial;
    g_prompt.user = user;
    g_prompt.str_cb = NULL;
    g_prompt.int_cb = cb;
    g_prompt.double_cb = NULL;
    g_prompt.calls++;
}

void
ui_prompt_open_double_async(const char* title, double initial, ui_prompt_double_done_fn cb, void* user) {
    DSD_SNPRINTF(g_prompt.title, sizeof g_prompt.title, "%s", title ? title : "");
    g_prompt.initial_double = initial;
    g_prompt.user = user;
    g_prompt.str_cb = NULL;
    g_prompt.int_cb = NULL;
    g_prompt.double_cb = cb;
    g_prompt.calls++;
}

void
ui_chooser_start_at(const char* title, const char* const* items, int count, int initial_sel,
                    void (*on_done)(void*, int), void* user) {
    DSD_SNPRINTF(g_chooser.title, sizeof g_chooser.title, "%s", title ? title : "");
    g_chooser.n = count;
    g_chooser.initial_sel = initial_sel;
    for (int i = 0; i < count && i < (int)(sizeof g_chooser.labels / sizeof g_chooser.labels[0]); i++) {
        g_chooser.labels[i] = items[i];
    }
    g_chooser.user = user;
    g_chooser.on_done = on_done;
    g_chooser.calls++;
}

void
ui_chooser_start(const char* title, const char* const* items, int count, void (*on_done)(void*, int), void* user) {
    ui_chooser_start_at(title, items, count, 0, on_done, user);
}

const dsdneoRuntimeConfig*
dsd_neo_get_config(void) {
    return g_cfg_valid ? &g_cfg : NULL;
}

const char*
dsd_user_config_default_path(void) {
    return g_default_config_path;
}

int
dsd_user_config_save_atomic(const char* path, const dsdneoUserConfig* cfg) {
    (void)path;
    (void)cfg;
    return g_save_atomic_rc;
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

int
dsd_user_config_list_profiles(const char* path, const char** names, char* names_buf, size_t names_buf_size,
                              int max_names) {
    (void)path;
    if (g_list_profiles_rc <= 0) {
        return g_list_profiles_rc;
    }
    assert(names_buf_size >= 16);
    assert(max_names >= 2);
    DSD_SNPRINTF(names_buf, names_buf_size, "base");
    DSD_SNPRINTF(names_buf + 8, names_buf_size - 8, "mobile");
    names[0] = names_buf;
    names[1] = names_buf + 8;
    return 2;
}

int
dsd_setenv(const char* name, const char* value, int overwrite) {
    (void)overwrite;
    DSD_SNPRINTF(g_env_name, sizeof g_env_name, "%s", name ? name : "");
    DSD_SNPRINTF(g_env_value, sizeof g_env_value, "%s", value ? value : "");
    g_env_set_calls++;
    return 0;
}

int
env_get_int(const char* name, int defv) {
    (void)name;
    (void)defv;
    return g_env_int_value;
}

double
env_get_double(const char* name, double defv) {
    (void)name;
    (void)defv;
    return g_env_double_value;
}

void
env_reparse_runtime_cfg(dsd_opts* opts) {
    (void)opts;
    g_reparse_calls++;
}

int
dsd_audio_enumerate_devices(dsd_audio_device* inputs, dsd_audio_device* outputs, int max_count) {
    if (g_audio_enum_rc < 0) {
        return g_audio_enum_rc;
    }
    if (max_count > 0) {
        inputs[0] = g_audio_inputs[0];
        outputs[0] = g_audio_outputs[0];
    }
    if (max_count > 1) {
        inputs[1] = g_audio_inputs[1];
        outputs[1] = g_audio_outputs[1];
    }
    return 0;
}

/* The RTL rows hand their prompt results to these; the rows themselves are what
 * is under test, so the callbacks only have to exist. */
void
cb_rtl_dev(void* u, int ok, int i) {
    (void)u;
    (void)ok;
    (void)i;
}

void
cb_rtl_freq(void* u, int ok, int f) {
    (void)u;
    (void)ok;
    (void)f;
}

void
cb_rtl_gain(void* u, int ok, int g) {
    (void)u;
    (void)ok;
    (void)g;
}

void
cb_rtl_ppm(void* u, int ok, int p) {
    (void)u;
    (void)ok;
    (void)p;
}

void
cb_rtl_bw(void* u, int ok, int bw) {
    (void)u;
    (void)ok;
    (void)bw;
}

void
cb_rtl_sql(void* u, int ok, double dB) {
    (void)u;
    (void)ok;
    (void)dB;
}

void
cb_rtl_vol(void* u, int ok, int m) {
    (void)u;
    (void)ok;
    (void)m;
}

void
cb_event_log_set(void* v, const char* path) {
    (void)v;
    (void)path;
}

void
cb_static_wav(void* v, const char* path) {
    (void)v;
    (void)path;
}

void
cb_raw_wav(void* v, const char* path) {
    (void)v;
    (void)path;
}

void
cb_dsp_out(void* v, const char* name) {
    (void)v;
    (void)name;
}

void
cb_config_load(void* v, const char* path) {
    (void)v;
    (void)path;
}

void
cb_config_save_as(void* v, const char* path) {
    (void)v;
    (void)path;
}

void
chooser_done_config_profile(void* u, int sel) {
    (void)u;
    (void)sel;
}

void
cb_setmod_bw(void* v, int ok, int bw) {
    (void)v;
    (void)ok;
    (void)bw;
}

void
cb_import_chan(void* v, const char* p) {
    (void)v;
    (void)p;
}

void
cb_import_group(void* v, const char* p) {
    (void)v;
    (void)p;
}

void
cb_tg_hold(void* v, int ok, int tg) {
    (void)v;
    (void)ok;
    (void)tg;
}

void
cb_hangtime(void* v, int ok, double s) {
    (void)v;
    (void)ok;
    (void)s;
}

void
cb_slot_pref(void* v, int ok, int p) {
    (void)v;
    (void)ok;
    (void)p;
}

void
cb_keys_dec(void* v, const char* p) {
    (void)v;
    (void)p;
}

void
cb_keys_hex(void* v, const char* p) {
    (void)v;
    (void)p;
}

void
cb_rr_account_user(void* v, const char* text) {
    (void)v;
    (void)text;
}

void
cb_rr_account_key(void* v, const char* text) {
    (void)v;
    (void)text;
}

void
rr_panel_open_import(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
rr_panel_open_library(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
cb_tyt_ap(void* v, const char* s) {
    (void)v;
    (void)s;
}

void
cb_retevis_rc2(void* v, const char* s) {
    (void)v;
    (void)s;
}

void
cb_tyt_ep(void* v, const char* s) {
    (void)v;
    (void)s;
}

void
cb_ken_scr(void* v, const char* s) {
    (void)v;
    (void)s;
}

void
cb_anytone_bp(void* v, const char* s) {
    (void)v;
    (void)s;
}

void
cb_xor_ks(void* v, const char* s) {
    (void)v;
    (void)s;
}

void
cb_p2_step(void* u, const char* text) {
    (void)u;
    (void)text;
}

void
cb_input_warn(void* v, int ok, double thr) {
    (void)v;
    (void)ok;
    (void)thr;
}

void
cb_audio_lpf(void* v, int ok, int hz) {
    (void)v;
    (void)ok;
    (void)hz;
}

void
cb_env_edit_name(void* u, const char* name) {
    (void)u;
    (void)name;
}

void
cb_auto_ppm_snr(void* v, int ok, double d) {
    (void)v;
    (void)ok;
    (void)d;
}

void
cb_auto_ppm_pwr(void* v, int ok, double d) {
    (void)v;
    (void)ok;
    (void)d;
}

void
cb_auto_ppm_zeroppm(void* v, int ok, double p) {
    (void)v;
    (void)ok;
    (void)p;
}

void
cb_auto_ppm_zerohz(void* v, int ok, int h) {
    (void)v;
    (void)ok;
    (void)h;
}

void
cb_tcp_prebuf(void* v, int ok, int ms) {
    (void)v;
    (void)ok;
    (void)ms;
}

void
cb_tcp_rcvbuf(void* v, int ok, int sz) {
    (void)v;
    (void)ok;
    (void)sz;
}

void
cb_tcp_rcvtimeo(void* v, int ok, int ms) {
    (void)v;
    (void)ok;
    (void)ms;
}

void
cb_io_save_symbol_capture(void* v, const char* path) {
    (void)v;
    (void)path;
}

void
chooser_done_pulse_out(void* u, int sel) {
    (void)u;
    (void)sel;
}

void
chooser_done_pulse_in(void* u, int sel) {
    (void)u;
    (void)sel;
}

void
cb_udp_out_host(void* u, const char* host) {
    (void)u;
    (void)host;
}

void
cb_tcp_host(void* u, const char* host) {
    (void)u;
    (void)host;
}

void
cb_gain_dig(void* u, int ok, double g) {
    (void)u;
    (void)ok;
    (void)g;
}

void
cb_gain_ana(void* u, int ok, double g) {
    (void)u;
    (void)ok;
    (void)g;
}

void
cb_input_vol(void* u, int ok, int m) {
    (void)u;
    (void)ok;
    (void)m;
}

void
cb_rig_host(void* u, const char* host) {
    (void)u;
    (void)host;
}

void
cb_switch_to_wav(void* v, const char* path) {
    (void)v;
    (void)path;
}

void
cb_switch_to_symbol(void* v, const char* path) {
    (void)v;
    (void)path;
}

void
cb_udp_in_addr(void* u, const char* addr) {
    (void)u;
    (void)addr;
}

void
cb_key_basic(void* v, int ok, int val) {
    (void)v;
    (void)ok;
    (void)val;
}

void
cb_hytera_step(void* u, const char* text) {
    (void)u;
    (void)text;
}

void
cb_key_scrambler(void* v, int ok, int val) {
    (void)v;
    (void)ok;
    (void)val;
}

void
cb_key_rc4des(void* v, const char* text) {
    (void)v;
    (void)text;
}

void
cb_aes_step(void* u, const char* text) {
    (void)u;
    (void)text;
}

void
cb_lr_custom(void* v, const char* path) {
    (void)v;
    (void)path;
}

void
cb_m17_user_data(void* u, const char* text) {
    (void)u;
    (void)text;
}

void
cb_set_p25_num(void* u, int ok, double val) {
    (void)u;
    (void)ok;
    (void)val;
}

static int
test_simple_commands_and_prompts(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    DSD_SNPRINTF(opts.event_out_file, sizeof opts.event_out_file, "events.log");
    DSD_SNPRINTF(opts.wav_out_file, sizeof opts.wav_out_file, "static.wav");
    DSD_SNPRINTF(opts.dsp_out_file, sizeof opts.dsp_out_file, "dsp");
    opts.setmod_bw = 12500;
    opts.slot_preference = 1;
    opts.slot1_on = 1;
    state.tg_hold = 1234;
    static UiCtx ctx;
    ctx = make_ctx(&opts, &state);

    reset_capture();
    act_toggle_invert(NULL);
    rc |= expect_int("invert command", g_cmd.id, DSD_APP_CMD_INVERT_TOGGLE);
    act_exit(NULL);
    rc |= expect_int("exit command", g_cmd.id, DSD_APP_CMD_QUIT);

    reset_capture();
    act_event_log_set(&ctx);
    rc |= expect_str("event prompt title", g_prompt.title, "Event log filename");
    rc |= expect_str("event prompt prefill", g_prompt.prefill, "events.log");
    rc |= expect_int("event prompt cap", (int)g_prompt.cap, 1024);

    reset_capture();
    act_static_wav(&ctx);
    rc |= expect_str("static wav prompt", g_prompt.prefill, "static.wav");

    reset_capture();
    act_dsp_out(&ctx);
    rc |= expect_str("dsp prompt", g_prompt.prefill, "dsp");
    rc |= expect_int("dsp prompt cap", (int)g_prompt.cap, 256);

    reset_capture();
    act_setmod_bw(&ctx);
    rc |= expect_str("setmod prompt", g_prompt.title, "Setmod BW (Hz)");
    rc |= expect_int("setmod initial", g_prompt.initial_int, 12500);

    reset_capture();
    act_tg_hold(&ctx);
    rc |= expect_int("tg hold initial", g_prompt.initial_int, 1234);

    reset_capture();
    act_slot_pref(&ctx);
    rc |= expect_int("slot pref initial", g_prompt.initial_int, 2);

    return rc;
}

static int
test_config_profile_and_env_actions(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    static UiCtx ctx;
    ctx = make_ctx(&opts, &state);

    reset_capture();
    DSD_SNPRINTF(state.config_autosave_path, sizeof state.config_autosave_path, "/tmp/current.toml");
    act_config_load(&ctx);
    rc |= expect_str("config load uses autosave", g_prompt.prefill, "/tmp/current.toml");

    reset_capture();
    act_config_save_current(&ctx);
    rc |= expect_int("save current snapshots", g_snapshot_calls, 1);
    rc |= expect_str("save current status", g_status, "Config saved to /tmp/current.toml");

    reset_capture();
    g_list_profiles_rc = 2;
    act_config_load_profile(&ctx);
    rc |= expect_str("profile chooser title", g_chooser.title, "Load Profile");
    rc |= expect_int("profile chooser count", g_chooser.n, 2);
    rc |= expect_str("profile label zero", g_chooser.labels[0], "base");
    free_profile_ctx((ProfileSelCtx*)g_chooser.user);

    reset_capture();
    g_cfg.deemph_mode = DSD_NEO_DEEMPH_OFF;
    act_deemph_cycle(&ctx);
    rc |= expect_str("deemph env", g_env_name, "DSD_NEO_DEEMPH");
    rc |= expect_str("deemph value", g_env_value, "50");
    rc |= expect_int("deemph reparse", g_reparse_calls, 1);

    reset_capture();
    opts.audio_in_type = AUDIO_IN_RTL;
    g_cfg.tcp_waitall_enable = 1;
    act_tcp_waitall(&ctx);
    rc |= expect_str("tcp waitall env", g_env_name, "DSD_NEO_TCP_WAITALL");
    rc |= expect_str("tcp waitall value", g_env_value, "0");
    rc |= expect_int("tcp waitall restart", g_cmd.id, DSD_APP_CMD_RTL_RESTART);

    reset_capture();
    act_auto_ppm_zerohz_prompt(&ctx);
    rc |= expect_int("auto ppm hz initial", g_prompt.initial_int, 77);
    rc |= expect_str("auto ppm hz title", g_prompt.title, "Auto-PPM zero-lock Hz");

    reset_capture();
    act_set_p25_min_follow(&ctx);
    rc |= expect_str("p25 prompt title", g_prompt.title, "P25: Min follow dwell (s)");
    rc |= expect_int("p25 prompt initial", g_prompt.initial_double == 12.5, 1);
    free(g_prompt.user);

    return rc;
}

static int
test_io_actions_and_choosers(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    DSD_SNPRINTF(opts.udp_hostname, sizeof opts.udp_hostname, "239.1.2.3");
    DSD_SNPRINTF(opts.tcp_hostname, sizeof opts.tcp_hostname, "tcp.local");
    DSD_SNPRINTF(opts.rigctlhostname, sizeof opts.rigctlhostname, "rig.local");
    DSD_SNPRINTF(opts.pa_output_idx, sizeof opts.pa_output_idx, "pulse0");
    opts.input_volume_multiplier = 16;
    static UiCtx ctx;
    ctx = make_ctx(&opts, &state);

    reset_capture();
    io_select_call_alert_events(&ctx);
    rc |= expect_str("call alert chooser", g_chooser.title, "Call Alert Events");
    rc |= expect_int("call alert choice count", g_chooser.n, 8);
    assert(g_chooser.on_done != NULL);
    g_chooser.on_done(g_chooser.user, 7);
    rc |= expect_int("call alert command", g_cmd.id, DSD_APP_CMD_CALL_ALERT_EVENTS_SET);
    rc |= expect_int("call alert all mask", g_cmd.data[0], DSD_CALL_ALERT_EVENT_ALL);

    reset_capture();
    opts.dmr_stereo_wav = 0;
    opts.wav_out_f = NULL;
    io_enable_per_call_wav(&ctx);
    rc |= expect_int("per-call wav inactive toggle command", g_cmd.id, DSD_APP_CMD_WAV_TOGGLE);
    rc |= expect_int("per-call wav inactive toggle leaves flag", opts.dmr_stereo_wav, 0);

    reset_capture();
    opts.dmr_stereo_wav = 1;
    opts.wav_out_f = (SNDFILE*)0x1;
    io_enable_per_call_wav(&ctx);
    rc |= expect_int("per-call wav active toggle command", g_cmd.id, DSD_APP_CMD_WAV_TOGGLE);
    rc |= expect_int("per-call wav active toggle leaves flag", opts.dmr_stereo_wav, 1);

    reset_capture();
    g_audio_outputs[0].initialized = 1;
    g_audio_outputs[0].index = 3;
    DSD_SNPRINTF(g_audio_outputs[0].name, sizeof g_audio_outputs[0].name, "out0");
    DSD_SNPRINTF(g_audio_outputs[0].description, sizeof g_audio_outputs[0].description, "Output Zero");
    io_set_pulse_out(&ctx);
    rc |= expect_str("pulse out chooser", g_chooser.title, "Select Pulse Output");
    rc |= expect_int("pulse out count", g_chooser.n, 1);
    rc |= expect_int("pulse out label has index", strstr(g_chooser.labels[0], "[3] out0") != NULL, 1);
    free_pulse_ctx((PulseSelCtx*)g_chooser.user);

    reset_capture();
    io_set_udp_out(&ctx);
    rc |= expect_str("udp out prompt prefill", g_prompt.prefill, "239.1.2.3");
    release_prompt_user();

    reset_capture();
    io_tcp_direct_link(&ctx);
    rc |= expect_str("tcp prompt prefill", g_prompt.prefill, "tcp.local");
    release_prompt_user();

    reset_capture();
    io_rigctl_config(&ctx);
    rc |= expect_str("rig prompt prefill", g_prompt.prefill, "rig.local");
    release_prompt_user();

    reset_capture();
    switch_out_pulse(&ctx);
    rc |= expect_int("pulse out command", g_cmd.id, DSD_APP_CMD_PULSE_OUT_SET);
    rc |= expect_str("pulse out payload", (const char*)g_cmd.data, "pulse0");

    return rc;
}

static int
test_key_lrrp_and_display_actions(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    DSD_SNPRINTF(state.m17dat, sizeof state.m17dat, "1,N0CALL,N1CALL");
    state.p2_wacn = 0xABCDE;
    static UiCtx ctx;
    ctx = make_ctx(&opts, &state);

    reset_capture();
    key_basic(&ctx);
    rc |= expect_str("basic key prompt", g_prompt.title, "Basic Privacy Key Number (DEC)");

    reset_capture();
    key_hytera(&ctx);
    rc |= expect_str("hytera prompt", g_prompt.title, "Hytera Privacy Key 1 (HEX)");
    release_prompt_user();

    reset_capture();
    key_aes(&ctx);
    rc |= expect_str("aes prompt", g_prompt.title, "AES Segment 1 (HEX) or 0");
    release_prompt_user();

    reset_capture();
    act_p2_params(&ctx);
    rc |= expect_str("p2 prompt prefill", g_prompt.prefill, "ABCDE");
    release_prompt_user();

    reset_capture();
    lr_home(&ctx);
    rc |= expect_int("lrrp home command", g_cmd.id, DSD_APP_CMD_LRRP_SET_HOME);

    reset_capture();
    act_m17_user_data(&ctx);
    rc |= expect_str("m17 prefill", g_prompt.prefill, "1,N0CALL,N1CALL");
    release_prompt_user();

    reset_capture();
    act_toggle_ui_p25_neighbors(&ctx);
    rc |= expect_int("ui neighbors command", g_cmd.id, DSD_APP_CMD_UI_SHOW_P25_NEIGHBORS_TOGGLE);

    reset_capture();
    act_toggle_ui_p25_metrics(&ctx);
    rc |= expect_int("ui metrics command", g_cmd.id, DSD_APP_CMD_UI_SHOW_P25_METRICS_TOGGLE);

    reset_capture();
    act_toggle_ui_p25_affil(&ctx);
    rc |= expect_int("ui affiliation command", g_cmd.id, DSD_APP_CMD_UI_SHOW_P25_AFFIL_TOGGLE);

    reset_capture();
    act_toggle_ui_p25_ga(&ctx);
    rc |= expect_int("ui grant activity command", g_cmd.id, DSD_APP_CMD_P25_GA_TOGGLE);

    reset_capture();
    act_toggle_ui_p25_iden(&ctx);
    rc |= expect_int("ui iden command", g_cmd.id, DSD_APP_CMD_UI_SHOW_P25_IDEN_TOGGLE);

    reset_capture();
    act_toggle_ui_p25_ccc(&ctx);
    rc |= expect_int("ui ccc command", g_cmd.id, DSD_APP_CMD_UI_SHOW_P25_CCC_TOGGLE);

    reset_capture();
    act_toggle_ui_channels(&ctx);
    rc |= expect_int("ui channels command", g_cmd.id, DSD_APP_CMD_UI_SHOW_CHANNELS_TOGGLE);

    reset_capture();
    act_toggle_ui_compact(&ctx);
    rc |= expect_int("ui compact command", g_cmd.id, DSD_APP_CMD_TOGGLE_COMPACT);

    reset_capture();
    act_toggle_ui_p25_callsign(&ctx);
    rc |= expect_int("ui callsign command", g_cmd.id, DSD_APP_CMD_UI_SHOW_P25_CALLSIGN_TOGGLE);

    reset_capture();
    switch_to_pulse(&ctx);
    rc |= expect_int("switch pulse command", g_cmd.id, DSD_APP_CMD_INPUT_SET_PULSE);

    reset_capture();
    switch_to_udp(&ctx);
    rc |= expect_str("udp input default", g_prompt.prefill, "127.0.0.1");
    release_prompt_user();

    return rc;
}

static int
test_additional_prompt_and_toggle_actions(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    DSD_SNPRINTF(opts.wav_out_file_raw, sizeof opts.wav_out_file_raw, "raw.wav");
    opts.trunk_hangtime = 2.75;
    opts.input_warn_db = -37.5;
    static UiCtx ctx;
    ctx = make_ctx(&opts, &state);

    reset_capture();
    act_toggle_payload(NULL);
    rc |= expect_int("payload toggle command", g_cmd.id, DSD_APP_CMD_PAYLOAD_TOGGLE);

    reset_capture();
    act_reset_eh(NULL);
    rc |= expect_int("event history reset command", g_cmd.id, DSD_APP_CMD_EH_RESET);

    reset_capture();
    act_event_log_disable(NULL);
    rc |= expect_int("event log disable command", g_cmd.id, DSD_APP_CMD_EVENT_LOG_DISABLE);

    reset_capture();
    act_raw_wav(&ctx);
    rc |= expect_str("raw wav prompt title", g_prompt.title, "Raw WAV filename");
    rc |= expect_str("raw wav prompt prefill", g_prompt.prefill, "raw.wav");

    reset_capture();
    act_import_chan(&ctx);
    rc |= expect_str("channel import prompt", g_prompt.title, "Channel map CSV");
    rc |= expect_int("channel import cap", (int)g_prompt.cap, 1024);

    reset_capture();
    act_import_group(&ctx);
    rc |= expect_str("group import prompt", g_prompt.title, "Group list CSV");

    reset_capture();
    act_keys_dec(&ctx);
    rc |= expect_str("keys dec prompt", g_prompt.title, "Keys CSV (DEC)");

    reset_capture();
    act_keys_hex(&ctx);
    rc |= expect_str("keys hex prompt", g_prompt.title, "Keys CSV (HEX)");

    reset_capture();
    act_tyt_ap(&ctx);
    rc |= expect_str("tyt ap prompt", g_prompt.title, "TYT AP string");
    rc |= expect_int("tyt ap cap", (int)g_prompt.cap, 256);

    reset_capture();
    act_retevis_rc2(&ctx);
    rc |= expect_str("retevis prompt", g_prompt.title, "Retevis AP string");

    reset_capture();
    act_tyt_ep(&ctx);
    rc |= expect_str("tyt ep prompt", g_prompt.title, "TYT EP string");

    reset_capture();
    act_ken_scr(&ctx);
    rc |= expect_str("kenwood prompt", g_prompt.title, "Kenwood scrambler");

    reset_capture();
    act_anytone_bp(&ctx);
    rc |= expect_str("anytone prompt", g_prompt.title, "Anytone BP");

    reset_capture();
    act_xor_ks(&ctx);
    rc |= expect_str("xor prompt", g_prompt.title, "XOR keystream");

    reset_capture();
    act_hangtime(&ctx);
    rc |= expect_str("hangtime prompt", g_prompt.title, "Hangtime seconds");
    rc |= expect_int("hangtime initial", g_prompt.initial_double == 2.75, 1);

    reset_capture();
    act_config_save_as(&ctx);
    rc |= expect_str("save as prompt", g_prompt.title, "Save config to path");
    rc |= expect_str("save as default path", g_prompt.prefill, "/tmp/default.toml");
    rc |= expect_int("save as prompt cap", (int)g_prompt.cap, 512);

    reset_capture();
    act_crc_relax(NULL);
    rc |= expect_int("crc relax command", g_cmd.id, DSD_APP_CMD_AGGR_SYNC_TOGGLE);

    reset_capture();
    act_trunk_toggle(NULL);
    rc |= expect_int("trunk toggle command", g_cmd.id, DSD_APP_CMD_TRUNK_TOGGLE);
    rc |= expect_str("trunk toggle status", g_status, "Trunking toggle requested...");

    reset_capture();
    act_scan_toggle(NULL);
    rc |= expect_int("scanner toggle command", g_cmd.id, DSD_APP_CMD_SCANNER_TOGGLE);
    rc |= expect_str("scanner toggle status", g_status, "Scanner toggle requested...");

    reset_capture();
    act_lcw_toggle(NULL);
    rc |= expect_int("lcw toggle command", g_cmd.id, DSD_APP_CMD_LCW_RETUNE_TOGGLE);

    reset_capture();
    act_p25_enc_lockout(NULL);
    rc |= expect_int("p25 enc lockout command", g_cmd.id, DSD_APP_CMD_TRUNK_ENC_TOGGLE);

    reset_capture();
    act_allow_toggle(NULL);
    rc |= expect_int("allow toggle command", g_cmd.id, DSD_APP_CMD_TRUNK_WLIST_TOGGLE);

    reset_capture();
    act_tune_group(NULL);
    rc |= expect_int("group tuning command", g_cmd.id, DSD_APP_CMD_TRUNK_GROUP_TOGGLE);

    reset_capture();
    act_tune_priv(NULL);
    rc |= expect_int("private tuning command", g_cmd.id, DSD_APP_CMD_TRUNK_PRIV_TOGGLE);

    reset_capture();
    act_tune_data(NULL);
    rc |= expect_int("data tuning command", g_cmd.id, DSD_APP_CMD_TRUNK_DATA_TOGGLE);

    reset_capture();
    act_rev_mute(NULL);
    rc |= expect_int("reverse mute command", g_cmd.id, DSD_APP_CMD_REVERSE_MUTE_TOGGLE);

    reset_capture();
    act_dmr_le(NULL);
    rc |= expect_int("dmr late entry command", g_cmd.id, DSD_APP_CMD_DMR_LE_TOGGLE);

    reset_capture();
    key_scrambler(&ctx);
    rc |= expect_str("scrambler prompt", g_prompt.title, "NXDN/dPMR Scrambler Key (DEC)");

    reset_capture();
    key_force_bp(NULL);
    rc |= expect_int("force basic privacy command", g_cmd.id, DSD_APP_CMD_FORCE_PRIV_TOGGLE);

    reset_capture();
    key_rc4des(&ctx);
    rc |= expect_str("rc4des prompt", g_prompt.title, "RC4/DES Key (HEX)");
    rc |= expect_int("rc4des prompt cap", (int)g_prompt.cap, 128);

    reset_capture();
    lr_dsdp(NULL);
    rc |= expect_int("lrrp dsdp command", g_cmd.id, DSD_APP_CMD_LRRP_SET_DSDP);

    reset_capture();
    lr_custom(&ctx);
    rc |= expect_str("lrrp custom prompt", g_prompt.title, "Enter LRRP output filename");

    reset_capture();
    lr_off(NULL);
    rc |= expect_int("lrrp disable command", g_cmd.id, DSD_APP_CMD_LRRP_DISABLE);

    reset_capture();
    act_set_input_warn(&ctx);
    rc |= expect_str("input warn prompt", g_prompt.title, "Low input warning threshold (dBFS)");
    rc |= expect_int("input warn opts initial", g_prompt.initial_double == -37.5, 1);

    reset_capture();
    act_set_input_warn(NULL);
    rc |= expect_int("input warn env fallback", g_prompt.initial_double == 12.5, 1);

    /* The squelch row has to be able to express "off". The prompt offered
     * pwr_to_dB() of a disabled squelch, which is -120: retyping what was shown
     * would have turned a squelch that was off into a real threshold. */
    reset_capture();
    opts.rtl_squelch_level = 0.0;
    rtl_set_sql(&ctx);
    rc |= expect_str("rtl squelch prompt", g_prompt.title, "Squelch (dB; 0 = off)");
    rc |= expect_int("rtl squelch off prompt offers off", g_prompt.initial_double == 0.0, 1);

    reset_capture();
    opts.rtl_squelch_level = pow(10.0, -5.0);
    rtl_set_sql(&ctx);
    rc |= expect_int("rtl squelch prompt states a real threshold", fabs(g_prompt.initial_double - (-50.0)) < 0.001, 1);

#if defined(__SSE__) || defined(__SSE2__)
    reset_capture();
    g_cfg.ftz_daz_enable = 0;
    act_toggle_ftz_daz(&ctx);
    rc |= expect_str("ftz env", g_env_name, "DSD_NEO_FTZ_DAZ");
    rc |= expect_str("ftz value", g_env_value, "1");
    rc |= expect_int("ftz reparse", g_reparse_calls, 1);
#endif

    reset_capture();
    g_cfg.audio_lpf_is_set = 1;
    g_cfg.audio_lpf_disable = 0;
    g_cfg.audio_lpf_cutoff_hz = 3200;
    act_set_audio_lpf(&ctx);
    rc |= expect_str("audio lpf prompt", g_prompt.title, "Audio LPF cutoff Hz (0=off)");
    rc |= expect_int("audio lpf initial", g_prompt.initial_int, 3200);

    reset_capture();
    g_cfg.audio_lpf_is_set = 1;
    g_cfg.audio_lpf_disable = 1;
    g_cfg.audio_lpf_cutoff_hz = 2800;
    act_set_audio_lpf(&ctx);
    rc |= expect_int("audio lpf disabled initial", g_prompt.initial_int, 0);

    reset_capture();
    act_window_freeze_toggle(&ctx);
    rc |= expect_str("window freeze env", g_env_name, "DSD_NEO_WINDOW_FREEZE");
    rc |= expect_str("window freeze value", g_env_value, "1");
    rc |= expect_int("window freeze reparse", g_reparse_calls, 1);

    reset_capture();
    g_cfg.auto_ppm_freeze_enable = 1;
    act_auto_ppm_freeze(&ctx);
    rc |= expect_str("auto ppm freeze env", g_env_name, "DSD_NEO_AUTO_PPM_FREEZE");
    rc |= expect_str("auto ppm freeze value", g_env_value, "0");

    reset_capture();
    act_auto_ppm_snr_prompt(&ctx);
    rc |= expect_str("auto ppm snr title", g_prompt.title, "Auto-PPM SNR threshold (dB)");
    rc |= expect_int("auto ppm snr initial", g_prompt.initial_double == 12.5, 1);

    reset_capture();
    act_auto_ppm_pwr_prompt(&ctx);
    rc |= expect_str("auto ppm power title", g_prompt.title, "Auto-PPM min power (dB)");
    rc |= expect_int("auto ppm power initial", g_prompt.initial_double == 12.5, 1);

    reset_capture();
    act_auto_ppm_zeroppm_prompt(&ctx);
    rc |= expect_str("auto ppm zeroppm title", g_prompt.title, "Auto-PPM zero-lock PPM");
    rc |= expect_int("auto ppm zeroppm initial", g_prompt.initial_double == 12.5, 1);

    reset_capture();
    act_set_p25_vc_grace(&ctx);
    rc |= expect_str("p25 vc grace prompt", g_prompt.title, "P25: VC grace seconds");
    rc |= expect_str("p25 vc grace env", ((P25NumCtx*)g_prompt.user)->name, "DSD_NEO_P25_VC_GRACE");
    release_prompt_user();

    reset_capture();
    act_set_p25_grant_voice(&ctx);
    rc |= expect_str("p25 grant voice prompt", g_prompt.title, "P25: Grant->Voice timeout (s)");
    rc |= expect_str("p25 grant voice env", ((P25NumCtx*)g_prompt.user)->name, "DSD_NEO_P25_GRANT_VOICE_TO");
    release_prompt_user();

    reset_capture();
    act_set_p25_cc_grace(&ctx);
    rc |= expect_str("p25 cc grace prompt", g_prompt.title, "P25: CC hunt grace (s)");
    rc |= expect_str("p25 cc grace env", ((P25NumCtx*)g_prompt.user)->name, "DSD_NEO_P25_CC_GRACE");
    release_prompt_user();

    reset_capture();
    act_set_p25_force_extra(&ctx);
    rc |= expect_str("p25 force extra prompt", g_prompt.title, "P25: Safety-net extra (s)");
    rc |= expect_str("p25 force extra env", ((P25NumCtx*)g_prompt.user)->name, "DSD_NEO_P25_FORCE_RELEASE_EXTRA");
    release_prompt_user();

    reset_capture();
    act_set_p25_force_margin(&ctx);
    rc |= expect_str("p25 force margin prompt", g_prompt.title, "P25: Safety-net margin (s)");
    rc |= expect_str("p25 force margin env", ((P25NumCtx*)g_prompt.user)->name, "DSD_NEO_P25_FORCE_RELEASE_MARGIN");
    release_prompt_user();

    reset_capture();
    act_set_p25_p1_err_pct(&ctx);
    rc |= expect_str("p25 p1 err pct prompt", g_prompt.title, "P25p1: Error-hold percent");
    rc |= expect_str("p25 p1 err pct env", ((P25NumCtx*)g_prompt.user)->name, "DSD_NEO_P25P1_ERR_HOLD_PCT");
    release_prompt_user();

    reset_capture();
    act_set_p25_p1_err_sec(&ctx);
    rc |= expect_str("p25 p1 err sec prompt", g_prompt.title, "P25p1: Error-hold seconds");
    rc |= expect_str("p25 p1 err sec env", ((P25NumCtx*)g_prompt.user)->name, "DSD_NEO_P25P1_ERR_HOLD_S");
    release_prompt_user();

    reset_capture();
    act_tcp_prebuf_prompt(&ctx);
    rc |= expect_str("tcp prebuf prompt", g_prompt.title, "RTL-TCP prebuffer (ms)");
    rc |= expect_int("tcp prebuf initial", g_prompt.initial_int, 77);

    reset_capture();
    act_tcp_rcvbuf_prompt(&ctx);
    rc |= expect_str("tcp rcvbuf prompt", g_prompt.title, "RTL-TCP SO_RCVBUF (0=default)");
    rc |= expect_int("tcp rcvbuf initial", g_prompt.initial_int, 77);

    reset_capture();
    act_tcp_rcvtimeo_prompt(&ctx);
    rc |= expect_str("tcp rcvtimeo prompt", g_prompt.title, "RTL-TCP SO_RCVTIMEO (ms; 0=off)");
    rc |= expect_int("tcp rcvtimeo initial", g_prompt.initial_int, 77);

    reset_capture();
    act_rt_sched(&ctx);
    rc |= expect_str("rt sched env", g_env_name, "DSD_NEO_RT_SCHED");
    rc |= expect_str("rt sched value", g_env_value, "1");

    reset_capture();
    g_cfg.mt_is_set = 1;
    g_cfg.mt_enable = 1;
    act_mt(&ctx);
    rc |= expect_str("mt env", g_env_name, "DSD_NEO_MT");
    rc |= expect_str("mt value", g_env_value, "0");

    reset_capture();
    act_env_editor(&ctx);
    rc |= expect_str("env editor prompt", g_prompt.title, "Enter DSD_NEO_* variable name");
    rc |= expect_str("env editor prefill", g_prompt.prefill, "DSD_NEO_");
    release_prompt_user();

    reset_capture();
    inv_x2(NULL);
    rc |= expect_int("x2 inversion command", g_cmd.id, DSD_APP_CMD_INV_X2_TOGGLE);

    reset_capture();
    inv_dmr(NULL);
    rc |= expect_int("dmr inversion command", g_cmd.id, DSD_APP_CMD_INV_DMR_TOGGLE);

    reset_capture();
    inv_dpmr(NULL);
    rc |= expect_int("dpmr inversion command", g_cmd.id, DSD_APP_CMD_INV_DPMR_TOGGLE);

    reset_capture();
    inv_m17(NULL);
    rc |= expect_int("m17 inversion command", g_cmd.id, DSD_APP_CMD_INV_M17_TOGGLE);

    reset_capture();
    io_replay_last_symbol_bin(NULL);
    rc |= expect_int("replay last command", g_cmd.id, DSD_APP_CMD_REPLAY_LAST);

    reset_capture();
    io_stop_symbol_playback(NULL);
    rc |= expect_int("stop playback command", g_cmd.id, DSD_APP_CMD_STOP_PLAYBACK);

    reset_capture();
    io_stop_symbol_saving(NULL);
    rc |= expect_int("stop symbol capture command", g_cmd.id, DSD_APP_CMD_SYMCAP_STOP);

    reset_capture();
    io_save_symbol_capture(&ctx);
    rc |= expect_str("save symbol prompt", g_prompt.title, "Enter Symbol Capture Filename");

    reset_capture();
    io_toggle_mute_enc(NULL);
    rc |= expect_int("all mutes toggle command", g_cmd.id, DSD_APP_CMD_ALL_MUTES_TOGGLE);

    reset_capture();
    io_toggle_call_alert(NULL);
    rc |= expect_int("call alert toggle command", g_cmd.id, DSD_APP_CMD_CALL_ALERT_TOGGLE);

    reset_capture();
    io_toggle_cc_candidates(NULL);
    rc |= expect_int("cc candidates command", g_cmd.id, DSD_APP_CMD_P25_CC_CAND_TOGGLE);

    reset_capture();
    io_set_gain_dig(&ctx);
    rc |= expect_str("digital gain prompt", g_prompt.title, "Digital output gain (0=auto; 1..50)");

    reset_capture();
    io_set_gain_ana(&ctx);
    rc |= expect_str("analog gain prompt", g_prompt.title, "Analog output gain (0..100)");

    reset_capture();
    io_toggle_monitor(NULL);
    rc |= expect_int("input monitor command", g_cmd.id, DSD_APP_CMD_INPUT_MONITOR_TOGGLE);

    reset_capture();
    io_toggle_cosine(NULL);
    rc |= expect_int("cosine filter command", g_cmd.id, DSD_APP_CMD_COSINE_FILTER_TOGGLE);

    reset_capture();
    opts.input_volume_multiplier = 0;
    io_set_input_volume(&ctx);
    rc |= expect_int("input volume prompt low clamp", g_prompt.initial_int, 1);

    reset_capture();
    opts.input_volume_multiplier = 20;
    io_set_input_volume(&ctx);
    rc |= expect_int("input volume prompt high clamp", g_prompt.initial_int, 16);

    reset_capture();
    switch_to_wav(&ctx);
    rc |= expect_str("switch wav prompt", g_prompt.title, "Enter WAV/RAW filename (or named pipe)");

    reset_capture();
    switch_to_symbol(&ctx);
    rc |= expect_str("switch symbol prompt", g_prompt.title, "Enter symbol .bin/.raw/.sym filename");

    reset_capture();
    DSD_SNPRINTF(opts.tcp_hostname, sizeof opts.tcp_hostname, "switch-tcp.local");
    io_tcp_direct_link(&ctx);
    rc |= expect_str("switch tcp prompt", g_prompt.prefill, "switch-tcp.local");
    release_prompt_user();

    reset_capture();
    DSD_SNPRINTF(opts.udp_hostname, sizeof opts.udp_hostname, "239.9.8.7");
    io_set_udp_out(&ctx);
    rc |= expect_str("switch out udp prompt", g_prompt.prefill, "239.9.8.7");
    release_prompt_user();

    reset_capture();
    switch_out_toggle_mute(NULL);
    rc |= expect_int("output mute toggle command", g_cmd.id, DSD_APP_CMD_TOGGLE_MUTE);

    return rc;
}

/*
 * Rows the signal-chain menu added for commands that were hotkey-only: each
 * posts exactly its command with no payload, the slot lockouts carry the slot,
 * and the decoder-mode picker posts the preset of the row chosen.
 */
static int
test_signal_chain_rows(void) {
    int rc = 0;

    static const struct {
        void (*fn)(void*);
        int cmd;
        const char* tag;
    } simple[] = {
        {act_mod_cycle, DSD_APP_CMD_MOD_TOGGLE, "modulation cycle"},
        {act_mod_p2_toggle, DSD_APP_CMD_MOD_P2_TOGGLE, "p25p2 modulation lock"},
        {act_lpf_toggle, DSD_APP_CMD_LPF_TOGGLE, "lpf"},
        {act_hpf_toggle, DSD_APP_CMD_HPF_TOGGLE, "hpf"},
        {act_pbf_toggle, DSD_APP_CMD_PBF_TOGGLE, "pbf"},
        {act_hpf_d_toggle, DSD_APP_CMD_HPF_D_TOGGLE, "digital hpf"},
        {act_slot1_toggle, DSD_APP_CMD_SLOT1_TOGGLE, "slot 1"},
        {act_slot2_toggle, DSD_APP_CMD_SLOT2_TOGGLE, "slot 2"},
        {act_dmr_reset, DSD_APP_CMD_DMR_RESET, "dmr reset"},
        {act_provoice_esk, DSD_APP_CMD_PROVOICE_ESK_TOGGLE, "provoice esk"},
        {act_provoice_mode, DSD_APP_CMD_PROVOICE_MODE_TOGGLE, "provoice mode"},
        {act_return_cc, DSD_APP_CMD_RETURN_CC, "return to cc"},
        {act_channel_cycle, DSD_APP_CMD_CHANNEL_CYCLE, "channel cycle"},
        {act_scan_hold_toggle, DSD_APP_CMD_SCAN_HOLD_TOGGLE, "scan hold"},
        {act_scan_avoid, DSD_APP_CMD_SCAN_AVOID, "scan avoid"},
        {act_scan_avoid_clear, DSD_APP_CMD_SCAN_AVOID_CLEAR, "scan avoid clear"},
        {act_force_rc4, DSD_APP_CMD_FORCE_RC4_TOGGLE, "force rc4"},
        {act_history_cycle, DSD_APP_CMD_HISTORY_CYCLE, "history cycle"},
        {act_eh_toggle_slot, DSD_APP_CMD_EH_TOGGLE_SLOT, "history slot"},
        {act_eh_prev, DSD_APP_CMD_EH_PREV, "history prev"},
        {act_eh_next, DSD_APP_CMD_EH_NEXT, "history next"},
        {act_sim_nocar, DSD_APP_CMD_SIM_NOCAR, "simulate no carrier"},
        {act_vis_const, DSD_APP_CMD_CONST_TOGGLE, "constellation"},
        {act_vis_const_norm, DSD_APP_CMD_CONST_NORM_TOGGLE, "constellation norm"},
        {act_vis_eye, DSD_APP_CMD_EYE_TOGGLE, "eye"},
        {act_vis_eye_unicode, DSD_APP_CMD_EYE_UNICODE_TOGGLE, "eye unicode"},
        {act_vis_eye_color, DSD_APP_CMD_EYE_COLOR_TOGGLE, "eye color"},
        {act_vis_fsk, DSD_APP_CMD_FSK_HIST_TOGGLE, "fsk histogram"},
        {act_vis_spectrum, DSD_APP_CMD_SPECTRUM_TOGGLE, "spectrum"},
    };

    for (size_t i = 0; i < sizeof simple / sizeof simple[0]; i++) {
        reset_capture();
        simple[i].fn(NULL);
        rc |= expect_int(simple[i].tag, g_cmd.id, simple[i].cmd);
        rc |= expect_int(simple[i].tag, (int)g_cmd.n, 0);
        rc |= expect_int(simple[i].tag, g_cmd.calls, 1);
    }

    reset_capture();
    act_lockout_slot1(NULL);
    rc |= expect_int("lockout slot 1 command", g_cmd.id, DSD_APP_CMD_LOCKOUT_SLOT);
    rc |= expect_int("lockout slot 1 payload size", (int)g_cmd.n, (int)sizeof(uint8_t));
    rc |= expect_int("lockout slot 1 payload", g_cmd.data[0], 0);
    reset_capture();
    act_lockout_slot2(NULL);
    rc |= expect_int("lockout slot 2 command", g_cmd.id, DSD_APP_CMD_LOCKOUT_SLOT);
    rc |= expect_int("lockout slot 2 payload", g_cmd.data[0], 1);

    reset_capture();
    act_decode_mode(NULL);
    rc |= expect_str("decode mode chooser title", g_chooser.title, "Decoder mode");
    rc |= expect_int("decode mode chooser count", g_chooser.n, 15);
    rc |= expect_str("decode mode first row is auto", g_chooser.labels[0], "m1");
    rc |= expect_str("decode mode second row is p25 both phases", g_chooser.labels[1], "m13");
    rc |= expect_str("decode mode fifth row is dmr", g_chooser.labels[4], "m4");
    rc |= expect_str("decode mode last row is analog", g_chooser.labels[14], "m14");
    rc |= expect_int("decode mode opens without posting", g_cmd.calls, 0);
    g_chooser.on_done(g_chooser.user, 4);
    rc |= expect_int("decode mode set command", g_cmd.id, DSD_APP_CMD_DECODE_MODE_SET);
    rc |= expect_int("decode mode set payload", cmd_i32(), (int)DSDCFG_MODE_DMR);

    reset_capture();
    act_decode_mode(NULL);
    g_chooser.on_done(g_chooser.user, -1);
    rc |= expect_int("decode mode cancel posts nothing", g_cmd.calls, 0);
    g_chooser.on_done(g_chooser.user, 15);
    rc |= expect_int("decode mode out-of-range posts nothing", g_cmd.calls, 0);

    /* The picker opens on the mode in effect. Row 0 is Auto, and Auto is the one
       choice the command layer never treats as a no-op, so opening there turned a
       second Enter into a full decoder reset. */
    static dsd_opts mode_opts;
    static dsd_state mode_state;
    DSD_MEMSET(&mode_opts, 0, sizeof mode_opts);
    DSD_MEMSET(&mode_state, 0, sizeof mode_state);
    static UiCtx mode_ctx;
    mode_ctx = make_ctx(&mode_opts, &mode_state);
    reset_capture();
    g_infer_mode = DSDCFG_MODE_P25P2;
    act_decode_mode(&mode_ctx);
    rc |= expect_int("decode mode opens on the live preset", g_chooser.initial_sel, 3);
    reset_capture();
    g_infer_mode = DSDCFG_MODE_AUTO;
    act_decode_mode(&mode_ctx);
    rc |= expect_int("decode mode opens on auto when auto is live", g_chooser.initial_sel, 0);

    return rc;
}

static int
test_config_and_pulse_failure_variants(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    static UiCtx ctx;
    ctx = make_ctx(&opts, &state);

    reset_capture();
    act_config_load(&ctx);
    rc |= expect_str("config load default path", g_prompt.prefill, "/tmp/default.toml");

    reset_capture();
    g_save_atomic_rc = -1;
    act_config_save_current(&ctx);
    rc |= expect_int("save current failure snapshots", g_snapshot_calls, 1);
    rc |= expect_str("save current failure status", g_status, "Failed to save config to /tmp/default.toml");

    reset_capture();
    act_config_save_default(&ctx);
    rc |= expect_int("save default metadata command", g_cmd.id, DSD_APP_CMD_CONFIG_METADATA_SET);
    rc |= expect_int("save default metadata count", g_config_metadata_calls, 1);
    rc |= expect_int("save default autosave enabled", g_config_metadata.autosave_enabled, 1);
    rc |= expect_str("save default autosave path", g_config_metadata.path, "/tmp/default.toml");
    rc |= expect_str("save default status", g_status, "Config saved to /tmp/default.toml");

    reset_capture();
    g_default_config_path = "";
    act_config_save_default(&ctx);
    rc |= expect_str("save default missing status", g_status, "No default config path; nothing saved");

    reset_capture();
    g_list_profiles_rc = -1;
    act_config_load_profile(&ctx);
    rc |= expect_str("profile read failure", g_status, "Failed to read profiles from /tmp/default.toml");

    reset_capture();
    g_list_profiles_rc = 0;
    act_config_load_profile(&ctx);
    rc |= expect_str("profile empty status", g_status, "No profiles found in /tmp/default.toml");

    reset_capture();
    g_audio_enum_rc = -1;
    io_set_pulse_in(&ctx);
    rc |= expect_str("pulse input enum failure", g_status, "Failed to get audio device list");

    reset_capture();
    io_set_pulse_out(&ctx);
    rc |= expect_str("pulse output empty status", g_status, "No Pulse outputs found");

    reset_capture();
    g_audio_inputs[0].initialized = 1;
    g_audio_inputs[0].index = 4;
    DSD_SNPRINTF(g_audio_inputs[0].name, sizeof g_audio_inputs[0].name, "in0");
    DSD_SNPRINTF(g_audio_inputs[0].description, sizeof g_audio_inputs[0].description, "Input Zero");
    io_set_pulse_in(&ctx);
    rc |= expect_str("pulse input chooser", g_chooser.title, "Select Pulse Input");
    rc |= expect_int("pulse input count", g_chooser.n, 1);
    rc |= expect_int("pulse input label has index", strstr(g_chooser.labels[0], "[4] in0") != NULL, 1);
    free_pulse_ctx((PulseSelCtx*)g_chooser.user);

    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_simple_commands_and_prompts();
    rc |= test_config_profile_and_env_actions();
    rc |= test_io_actions_and_choosers();
    rc |= test_key_lrrp_and_display_actions();
    rc |= test_additional_prompt_and_toggle_actions();
    rc |= test_signal_chain_rows();
    rc |= test_config_and_pulse_failure_variants();
    if (rc == 0) {
        printf("UI_MENU_ACTIONS: OK\n");
    }
    return rc;
}
