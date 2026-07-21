// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Frontend → decoder app-command queue (bounded). */

#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/history.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/frontend_types.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/crypto/dmr_keystream.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/io/control.h>
#include <dsd-neo/io/rigctl_client.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/io/udp_input.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/freq_parse.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/telemetry.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <sndfile.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "command_dispatch.h"
#include "commands_internal.h"
#include "dsd-neo/core/dibit.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/platform.h"
#include "dsd-neo/platform/sockets.h"
#include "dsd-neo/runtime/call_alert.h"
#include "services.h"

#ifdef USE_RADIO
#endif

#define DSD_APP_CMD_Q_CAP 128

_Static_assert(sizeof(dsdneoUserConfig) <= sizeof(((struct dsd_app_command*)0)->data),
               "dsd_app_command payload too small for dsdneoUserConfig");

enum {
    UI_CMD_APPLY_UNHANDLED = 0,
    UI_CMD_APPLY_COMPLETED = 1,
    UI_CMD_APPLY_FAILED = 2,
    UI_CMD_APPLY_UNSUPPORTED = 3,
    UI_CMD_APPLY_INVALID_PAYLOAD = 4,
    UI_CMD_APPLY_RESTART_REQUIRED = 5,
};

static struct dsd_app_command g_q[DSD_APP_CMD_Q_CAP];
static size_t g_head = 0; // pop index
static size_t g_tail = 0; // push index
static dsd_mutex_t g_mu;
static atomic_int g_mu_init = 0;
static atomic_int g_overflow = 0;
static atomic_int g_overflow_warn_gate = 0;

static void
ensure_mu_init(void) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_mu_init, &expected, 1)) {
        dsd_mutex_init(&g_mu);
    }
}

// Dispatch commands via per-domain registries
static int
ui_cmd_dispatch(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    const struct dsd_app_command_reg* regs[] = {dsd_app_actions_audio, dsd_app_actions_radio, dsd_app_actions_trunk,
                                                dsd_app_actions_logging, NULL};
    for (int i = 0; regs[i] != NULL; ++i) {
        for (const struct dsd_app_command_reg* r = regs[i]; r && r->fn != NULL; ++r) {
            if (r->id == c->id) {
                return r->fn(opts, state, c);
            }
        }
    }
    return 0; // not handled
}

static inline int
q_is_full_unlocked(void) {
    return ((g_tail + 1) % DSD_APP_CMD_Q_CAP) == g_head;
}

static inline int
q_is_empty_unlocked(void) {
    return g_head == g_tail;
}

static const int k_ui_cmd_string_ids[] = {
    DSD_APP_CMD_EVENT_LOG_SET,     DSD_APP_CMD_WAV_STATIC_OPEN,      DSD_APP_CMD_WAV_RAW_OPEN,
    DSD_APP_CMD_DSP_OUT_SET,       DSD_APP_CMD_SYMCAP_OPEN,          DSD_APP_CMD_SYMBOL_IN_OPEN,
    DSD_APP_CMD_INPUT_WAV_SET,     DSD_APP_CMD_INPUT_SYM_STREAM_SET, DSD_APP_CMD_PULSE_OUT_SET,
    DSD_APP_CMD_PULSE_IN_SET,      DSD_APP_CMD_LRRP_SET_CUSTOM,      DSD_APP_CMD_IMPORT_CHANNEL_MAP,
    DSD_APP_CMD_IMPORT_GROUP_LIST, DSD_APP_CMD_IMPORT_KEYS_DEC,      DSD_APP_CMD_IMPORT_KEYS_HEX,
    DSD_APP_CMD_KEY_TYT_AP_SET,    DSD_APP_CMD_KEY_RETEVIS_RC2_SET,  DSD_APP_CMD_KEY_TYT_EP_SET,
    DSD_APP_CMD_KEY_KEN_SCR_SET,   DSD_APP_CMD_KEY_ANYTONE_BP_SET,   DSD_APP_CMD_KEY_XOR_SET,
    DSD_APP_CMD_M17_USER_DATA_SET,
};

static int
ui_cmd_is_coalescible_setter(int cmd_id) {
    switch (cmd_id) {
        case DSD_APP_CMD_GAIN_SET:
        case DSD_APP_CMD_AGAIN_SET:
        case DSD_APP_CMD_INPUT_VOL_SET:
        case DSD_APP_CMD_RTL_SET_FREQ:
        case DSD_APP_CMD_RTL_SET_GAIN:
        case DSD_APP_CMD_RTL_SET_PPM:
        case DSD_APP_CMD_RTL_SET_BW:
        case DSD_APP_CMD_RTL_SET_SQL_DB:
        case DSD_APP_CMD_RTL_SET_VOL_MULT:
        case DSD_APP_CMD_HANGTIME_SET:
        case DSD_APP_CMD_SLOT_PREF_SET: return 1;
        default: return 0;
    }
}

static int
ui_cmd_is_string_payload_id(int cmd_id) {
    for (size_t i = 0; i < sizeof k_ui_cmd_string_ids / sizeof k_ui_cmd_string_ids[0]; i++) {
        if (k_ui_cmd_string_ids[i] == cmd_id) {
            return 1;
        }
    }
    return 0;
}

static struct dsd_app_command*
ui_cmd_find_pending_tail_unlocked(int cmd_id) {
    if (q_is_empty_unlocked()) {
        return NULL;
    }
    const size_t tail_idx = (g_tail + DSD_APP_CMD_Q_CAP - 1U) % DSD_APP_CMD_Q_CAP;
    return g_q[tail_idx].id == cmd_id ? &g_q[tail_idx] : NULL;
}

static void
ui_cmd_store_payload(struct dsd_app_command* c, int cmd_id, const void* payload, size_t payload_sz) {
    size_t copy_sz = payload_sz;
    if (copy_sz > sizeof c->data) {
        copy_sz = sizeof c->data;
    }
    c->id = cmd_id;
    c->n = copy_sz;
    c->payload_truncated = (payload_sz > copy_sz) ? 1U : 0U;
    if (copy_sz && payload) {
        DSD_MEMCPY(c->data, payload, copy_sz);
        if (c->payload_truncated && ui_cmd_is_string_payload_id(cmd_id)) {
            c->data[copy_sz - 1U] = '\0';
        }
    }
}

static void ui_set_toast(dsd_state* state, int ttl_s, const char* fmt, ...) DSD_ATTR_FORMAT(printf, 3, 4);

static void
ui_set_toast(dsd_state* state, int ttl_s, const char* fmt, ...) {
    if (!state || !fmt) {
        return;
    }
    if (ttl_s < 1) {
        ttl_s = 1;
    }
    va_list ap;
    va_start(ap, fmt);
    (void)DSD_VSNPRINTF(state->ui_msg, sizeof state->ui_msg, fmt, ap);
    va_end(ap);
    state->ui_msg_expire = time(NULL) + ttl_s;
}

static int
ui_reconfigure_output_for_input_policy(dsd_opts* opts, dsd_state* state) {
    if (dsd_audio_reconfigure_output_for_input_policy(opts) != 0) {
        ui_set_toast(state, 4, "Failed: audio output reconfigure");
        return -1;
    }
    return 0;
}

static void
ui_set_tcp_audio_connected_toast_if_output_ready(dsd_opts* opts, dsd_state* state, const char* host, int port) {
    if (ui_reconfigure_output_for_input_policy(opts, state) != 0) {
        return;
    }
    ui_set_toast(state, 3, "TCP audio connected: %s:%d", host, port);
}

#ifdef USE_RADIO
static inline int
ui_rc_is_not_supported(int rc) {
    return rc == DSD_ERR_NOT_SUPPORTED;
}
#endif

static int
ui_cmd_apply_status_from_service_rc(int rc) {
    if (rc == 0) {
        return UI_CMD_APPLY_COMPLETED;
    }
    if (rc == DSD_ERR_NOT_SUPPORTED) {
        return UI_CMD_APPLY_UNSUPPORTED;
    }
    return UI_CMD_APPLY_FAILED;
}

#ifdef USE_RADIO
static int
ui_cmd_apply_status_from_tune_rc(int rc) {
    if (rc == RTL_STREAM_TUNE_TIMEOUT) {
        /* The controller accepted the tune and will publish its terminal
         * result after the synchronous wait expires. Command completion here
         * means the request was accepted, not that hardware already moved. */
        return UI_CMD_APPLY_COMPLETED;
    }
    return ui_cmd_apply_status_from_service_rc(rc);
}
#endif

struct UiVisibilityToggleSpec {
    int cmd_id;
    size_t opts_offset;
};

static const struct UiVisibilityToggleSpec k_ui_visibility_toggle_specs[] = {
    {DSD_APP_CMD_UI_SHOW_DSP_PANEL_TOGGLE, offsetof(dsd_opts, frontend_display.show_dsp_panel)},
    {DSD_APP_CMD_UI_SHOW_P25_METRICS_TOGGLE, offsetof(dsd_opts, frontend_display.show_p25_metrics)},
    {DSD_APP_CMD_UI_SHOW_P25_AFFIL_TOGGLE, offsetof(dsd_opts, frontend_display.show_p25_affiliations)},
    {DSD_APP_CMD_UI_SHOW_P25_NEIGHBORS_TOGGLE, offsetof(dsd_opts, frontend_display.show_p25_neighbors)},
    {DSD_APP_CMD_UI_SHOW_P25_IDEN_TOGGLE, offsetof(dsd_opts, frontend_display.show_p25_iden_plan)},
    {DSD_APP_CMD_UI_SHOW_P25_CCC_TOGGLE, offsetof(dsd_opts, frontend_display.show_p25_cc_candidates)},
    {DSD_APP_CMD_UI_SHOW_CHANNELS_TOGGLE, offsetof(dsd_opts, frontend_display.show_channels)},
    {DSD_APP_CMD_UI_SHOW_P25_CALLSIGN_TOGGLE, offsetof(dsd_opts, frontend_display.show_p25_callsign_decode)},
};

static int
ui_apply_visibility_toggle(dsd_opts* opts, int cmd_id) {
    for (size_t i = 0; i < (sizeof k_ui_visibility_toggle_specs / sizeof k_ui_visibility_toggle_specs[0]); ++i) {
        if (k_ui_visibility_toggle_specs[i].cmd_id == cmd_id) {
            uint8_t* field = (uint8_t*)((char*)opts + k_ui_visibility_toggle_specs[i].opts_offset);
            *field = *field ? 0 : 1;
            return 1;
        }
    }
    return 0;
}

static int
apply_cmd_ui_visibility(dsd_opts* opts, const struct dsd_app_command* c) {
    if (!opts || !c) {
        return 0;
    }
    return ui_apply_visibility_toggle(opts, c->id);
}

static int
ui_cmd_copy_payload_string(const struct dsd_app_command* c, char* out, size_t out_sz) {
    if (!c || !out || out_sz == 0 || c->n == 0) {
        return 0;
    }
    size_t n = (c->n < out_sz - 1) ? c->n : out_sz - 1;
    DSD_MEMCPY(out, c->data, n);
    out[n] = '\0';
    return 1;
}

typedef int (*dsd_app_command_handler_fn)(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c);

struct dsd_app_command_handler_entry {
    int cmd_id;
    dsd_app_command_handler_fn fn;
};

static int
ui_cmd_apply_handler_table(const struct dsd_app_command_handler_entry* entries, size_t count, dsd_opts* opts,
                           dsd_state* state, const struct dsd_app_command* c) {
    if (!entries || !c) {
        return 0;
    }
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].cmd_id == c->id) {
            return entries[i].fn(opts, state, c);
        }
    }
    return 0;
}

static void
ui_cmd_reset_key_mute_state(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return;
    }
    state->keyloader = 0;
    state->payload_keyid = state->payload_keyidR = 0;
    opts->dmr_mute_encL = opts->dmr_mute_encR = 0;
}

static int
apply_cmd_key_management_basic(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    if (!opts || !state || !c) {
        return 0;
    }
    switch (c->id) {
        case DSD_APP_CMD_KEY_BASIC_SET: {
            if (c->n >= (int)sizeof(uint32_t)) {
                uint32_t v = 0;
                DSD_MEMCPY(&v, c->data, sizeof v);
                state->K = v;
                ui_cmd_reset_key_mute_state(opts, state);
            }
            return 1;
        }
        case DSD_APP_CMD_KEY_SCRAMBLER_SET: {
            if (c->n >= (int)sizeof(uint32_t)) {
                uint32_t v = 0;
                DSD_MEMCPY(&v, c->data, sizeof v);
                state->R = v;
                ui_cmd_reset_key_mute_state(opts, state);
            }
            return 1;
        }
        case DSD_APP_CMD_KEY_RC4DES_SET: {
            if (c->n >= (int)sizeof(uint64_t)) {
                uint64_t v = 0;
                DSD_MEMCPY(&v, c->data, sizeof v);
                state->R = v;
                state->RR = v;
                ui_cmd_reset_key_mute_state(opts, state);
                p25_sm_clear_encrypted_call_cache(state);
            }
            return 1;
        }
        default: return 0;
    }
}

static int
apply_cmd_key_hytera_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    if (!opts || !state || !c || c->n < (int)(sizeof(uint64_t) * 5)) {
        return 0;
    }

    struct {
        uint64_t H, K1, K2, K3, K4;
    } p;

    DSD_MEMCPY(&p, c->data, sizeof p);
    state->H = p.H;
    state->K1 = p.K1;
    state->K2 = p.K2;
    state->K3 = p.K3;
    state->K4 = p.K4;
    if (state->K1 == 0ULL && state->K2 == 0ULL && state->K3 == 0ULL && state->K4 == 0ULL) {
        state->hytera_key_segments = 0U;
    } else if (state->K3 != 0ULL || state->K4 != 0ULL) {
        state->hytera_key_segments = 4U;
    } else if (state->K2 != 0ULL) {
        state->hytera_key_segments = 2U;
    } else {
        state->hytera_key_segments = 1U;
    }
    ui_cmd_reset_key_mute_state(opts, state);
    DSD_SNPRINTF(state->ui_msg, sizeof state->ui_msg, "Hytera key loaded (%s)",
                 (state->M == 1) ? "forced" : "not forced");
    state->ui_msg_expire = time(NULL) + 5;
    return 1;
}

static int
apply_cmd_key_aes_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    if (!opts || !state || !c || c->n < (int)(sizeof(uint64_t) * 4)) {
        return 0;
    }

    struct {
        uint64_t K1, K2, K3, K4;
    } p;

    DSD_MEMCPY(&p, c->data, sizeof p);
    state->A1[0] = state->A1[1] = p.K1;
    state->A2[0] = state->A2[1] = p.K2;
    state->A3[0] = state->A3[1] = p.K3;
    state->A4[0] = state->A4[1] = p.K4;
    state->aes_key_loaded[0] = state->aes_key_loaded[1] =
        (p.K1 != 0ULL || p.K2 != 0ULL || p.K3 != 0ULL || p.K4 != 0ULL) ? 1 : 0;
    state->aes_key_segments[0] = state->aes_key_segments[1] = 4U;
    for (int i = 0; i < 8; i++) {
        state->aes_key[i + 0] = (uint8_t)((p.K1 >> (56 - (i * 8))) & 0xFFU);
        state->aes_key[i + 8] = (uint8_t)((p.K2 >> (56 - (i * 8))) & 0xFFU);
        state->aes_key[i + 16] = (uint8_t)((p.K3 >> (56 - (i * 8))) & 0xFFU);
        state->aes_key[i + 24] = (uint8_t)((p.K4 >> (56 - (i * 8))) & 0xFFU);
    }
    state->H = 0ULL;
    state->K1 = 0ULL;
    state->K2 = 0ULL;
    state->K3 = 0ULL;
    state->K4 = 0ULL;
    state->hytera_key_segments = 0U;
    ui_cmd_reset_key_mute_state(opts, state);
    p25_sm_clear_encrypted_call_cache(state);
    return 1;
}

static int
apply_cmd_key_management_block_keys(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    static const struct dsd_app_command_handler_entry entries[] = {
        {DSD_APP_CMD_KEY_HYTERA_SET, apply_cmd_key_hytera_set},
        {DSD_APP_CMD_KEY_AES_SET, apply_cmd_key_aes_set},
    };
    return ui_cmd_apply_handler_table(entries, sizeof entries / sizeof entries[0], opts, state, c);
}

typedef void (*UiStreamKeyLoaderFn)(dsd_state* state, const char* input, int show_keys);

struct UiStreamKeyLoaderEntry {
    int cmd_id;
    size_t payload_cap;
    UiStreamKeyLoaderFn fn;
};

static void
ui_load_ken_scrambler_key(dsd_state* state, const char* input, int show_keys) {
    char local[128];
    DSD_SNPRINTF(local, sizeof local, "%s", input ? input : "");
    ken_dmr_scrambler_keystream_creation(state, local, show_keys);
}

static void
ui_load_anytone_bp_key(dsd_state* state, const char* input, int show_keys) {
    char local[128];
    DSD_SNPRINTF(local, sizeof local, "%s", input ? input : "");
    anytone_bp_keystream_creation(state, local, show_keys);
}

static int
apply_cmd_key_management_stream_keys(const dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    static const struct UiStreamKeyLoaderEntry entries[] = {
        {DSD_APP_CMD_KEY_TYT_AP_SET, 256U, tyt_ap_pc4_keystream_creation},
        {DSD_APP_CMD_KEY_RETEVIS_RC2_SET, 256U, retevis_rc2_keystream_creation},
        {DSD_APP_CMD_KEY_TYT_EP_SET, 256U, tyt_ep_aes_keystream_creation},
        {DSD_APP_CMD_KEY_KEN_SCR_SET, 128U, ui_load_ken_scrambler_key},
        {DSD_APP_CMD_KEY_ANYTONE_BP_SET, 128U, ui_load_anytone_bp_key},
        {DSD_APP_CMD_KEY_XOR_SET, 256U, straight_mod_xor_keystream_creation},
    };
    if (!opts || !state || !c) {
        return 0;
    }

    for (size_t i = 0U; i < sizeof entries / sizeof entries[0]; i++) {
        if (entries[i].cmd_id == c->id) {
            char s[256];
            if (ui_cmd_copy_payload_string(c, s, entries[i].payload_cap)) {
                entries[i].fn(state, s, opts->show_keys);
            }
            return 1;
        }
    }
    return 0;
}

static int
apply_cmd_key_management_m17(dsd_state* state, const struct dsd_app_command* c) {
    if (!state || !c || c->id != DSD_APP_CMD_M17_USER_DATA_SET) {
        return 0;
    }
    if (c->n > 0) {
        size_t n = (c->n < sizeof(state->m17dat) - 1) ? c->n : sizeof(state->m17dat) - 1;
        DSD_MEMCPY(state->m17dat, c->data, n);
        state->m17dat[n] = '\0';
    }
    return 1;
}

static int
apply_cmd_key_management(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    if (!c) {
        return 0;
    }
    if (apply_cmd_key_management_basic(opts, state, c)) {
        return 1;
    }
    if (apply_cmd_key_management_block_keys(opts, state, c)) {
        return 1;
    }
    if (apply_cmd_key_management_stream_keys(opts, state, c)) {
        return 1;
    }
    return apply_cmd_key_management_m17(state, c);
}

#ifdef USE_RADIO
static int
apply_dsp_op_cqpsk_toggle(const dsd_app_dsp_payload* p) {
    if (!p || p->op != DSD_APP_DSP_OP_TOGGLE_CQ) {
        return 0;
    }
    int cq = 0;
    rtl_stream_get_cqpsk_status(&cq, NULL);
    rtl_stream_toggle_cqpsk(cq ? 0 : 1);
    return 1;
}

static int
apply_dsp_op_iq_and_ted(const dsd_app_dsp_payload* p) {
    if (!p) {
        return 0;
    }
    switch (p->op) {
        case DSD_APP_DSP_OP_TOGGLE_IQBAL: {
            int on = rtl_stream_get_iq_balance();
            int new_on = on ? 0 : 1;
            rtl_stream_toggle_iq_balance(new_on);
            dsd_setenv("DSD_NEO_IQ_BALANCE", new_on ? "1" : "0", 1);
            return 1;
        }
        case DSD_APP_DSP_OP_IQ_DC_TOGGLE: {
            int k = 0;
            int on = rtl_stream_get_iq_dc(&k);
            int new_on = on ? 0 : 1;
            rtl_stream_set_iq_dc(new_on, -1);
            dsd_setenv("DSD_NEO_IQ_DC_BLOCK", new_on ? "1" : "0", 1);
            return 1;
        }
        case DSD_APP_DSP_OP_IQ_DC_K_DELTA: {
            int k = 0;
            (void)rtl_stream_get_iq_dc(&k);
            rtl_stream_set_iq_dc(-1, k + p->a);
            return 1;
        }
        case DSD_APP_DSP_OP_TED_GAIN_SET: {
            int g_milli = p->a;
            if (g_milli < 10) {
                g_milli = 10;
            }
            if (g_milli > 500) {
                g_milli = 500;
            }
            rtl_stream_set_ted_gain((float)g_milli * 0.001f);
            return 1;
        }
        default: return 0;
    }
}

static int
apply_dsp_op_frontend_gain(const dsd_app_dsp_payload* p) {
    if (!p) {
        return 0;
    }
    if (p->op == DSD_APP_DSP_OP_TUNER_AUTOGAIN_TOGGLE) {
        int on = rtl_stream_get_tuner_autogain();
        rtl_stream_set_tuner_autogain(on ? 0 : 1);
        return 1;
    }
    return 0;
}

static void
apply_dsp_op(const dsd_app_dsp_payload* p) {
    if (!p) {
        return;
    }
    if (apply_dsp_op_cqpsk_toggle(p)) {
        return;
    }
    if (apply_dsp_op_iq_and_ted(p)) {
        return;
    }
    (void)apply_dsp_op_frontend_gain(p);
}
#endif

static int
apply_cmd_runtime_toggles(dsd_opts* opts, const struct dsd_app_command* c) {
    if (!opts || !c) {
        return 0;
    }
    switch (c->id) {
        case DSD_APP_CMD_DMR_LE_TOGGLE: svc_toggle_dmr_le(opts); return 1;
        case DSD_APP_CMD_ALL_MUTES_TOGGLE: svc_toggle_all_mutes(opts); return 1;
        case DSD_APP_CMD_INV_X2_TOGGLE: svc_toggle_inv_x2(opts); return 1;
        case DSD_APP_CMD_INV_DMR_TOGGLE: svc_toggle_inv_dmr(opts); return 1;
        case DSD_APP_CMD_INV_DPMR_TOGGLE: svc_toggle_inv_dpmr(opts); return 1;
        case DSD_APP_CMD_INV_M17_TOGGLE: svc_toggle_inv_m17(opts); return 1;
        default: return 0;
    }
}

static int
ui_cmd_handle_wav_static_open(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && c->n > 0) {
        char path[1024] = {0};
        if (ui_cmd_copy_payload_string(c, path, sizeof path)) {
            int rc = svc_open_static_wav(opts, state, path);
            result = ui_cmd_apply_status_from_service_rc(rc);
            if (rc == 0) {
                ui_set_toast(state, 3, "Applied: Static WAV output -> %s", path);
            } else {
                ui_set_toast(state, 4, "Failed: Static WAV open -> %s", path);
            }
        }
    }
    return result;
}

static int
ui_cmd_handle_wav_raw_open(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && c->n > 0) {
        char path[1024] = {0};
        if (ui_cmd_copy_payload_string(c, path, sizeof path)) {
            int rc = svc_open_raw_wav(opts, state, path);
            result = ui_cmd_apply_status_from_service_rc(rc);
            if (rc == 0) {
                ui_set_toast(state, 3, "Applied: Raw WAV output -> %s", path);
            } else {
                ui_set_toast(state, 4, "Failed: Raw WAV open -> %s", path);
            }
        }
    }
    return result;
}

static int
ui_cmd_handle_dsp_out_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int result = UI_CMD_APPLY_COMPLETED;
    if (c->n > 0) {
        char name[256] = {0};
        if (ui_cmd_copy_payload_string(c, name, sizeof name)) {
            int rc = svc_set_dsp_output_file(opts, name);
            result = ui_cmd_apply_status_from_service_rc(rc);
            if (rc == 0) {
                ui_set_toast(state, 3, "Applied: DSP output -> %s", opts->dsp_out_file);
            } else {
                ui_set_toast(state, 4, "Failed: DSP output path invalid");
            }
        }
    }
    return result;
}

static int
apply_cmd_io_and_import_file_outputs_a(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    static const struct dsd_app_command_handler_entry k_handlers[] = {
        {DSD_APP_CMD_WAV_STATIC_OPEN, ui_cmd_handle_wav_static_open},
        {DSD_APP_CMD_WAV_RAW_OPEN, ui_cmd_handle_wav_raw_open},
        {DSD_APP_CMD_DSP_OUT_SET, ui_cmd_handle_dsp_out_set},
    };
    if (!opts || !c) {
        return 0;
    }
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

static int
ui_cmd_handle_symcap_open(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && c->n > 0) {
        char path[1024] = {0};
        if (ui_cmd_copy_payload_string(c, path, sizeof path)) {
            int rc = svc_open_symbol_out(opts, state, path);
            result = ui_cmd_apply_status_from_service_rc(rc);
            if (rc == 0) {
                ui_set_toast(state, 3, "Applied: Symbol capture -> %s", path);
            } else {
                ui_set_toast(state, 4, "Failed: Symbol capture open -> %s", path);
            }
        }
    }
    return result;
}

static int
ui_cmd_handle_symbol_in_open(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && c->n > 0) {
        char path[1024] = {0};
        if (ui_cmd_copy_payload_string(c, path, sizeof path)) {
            int rc = svc_open_symbol_in(opts, state, path);
            result = ui_cmd_apply_status_from_service_rc(rc);
            if (rc == 0) {
                if (ui_reconfigure_output_for_input_policy(opts, state) == 0) {
                    ui_set_toast(state, 3, "Applied: Symbol input -> %s", path);
                } else {
                    result = UI_CMD_APPLY_FAILED;
                }
            } else {
                ui_set_toast(state, 4, "Failed: Symbol input open -> %s", path);
            }
        }
    }
    return result;
}

static int
ui_cmd_handle_input_wav_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    if (c->n > 0 && ui_cmd_copy_payload_string(c, opts->audio_in_dev, sizeof opts->audio_in_dev)) {
        opts->audio_in_type = AUDIO_IN_WAV;
        if (ui_reconfigure_output_for_input_policy(opts, state) == 0) {
            ui_set_toast(state, 3, "Applied: WAV input -> %s", opts->audio_in_dev);
        } else {
            return UI_CMD_APPLY_FAILED;
        }
    }
    return UI_CMD_APPLY_COMPLETED;
}

static int
ui_cmd_handle_input_sym_stream_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    if (c->n > 0 && ui_cmd_copy_payload_string(c, opts->audio_in_dev, sizeof opts->audio_in_dev)) {
        opts->audio_in_type = AUDIO_IN_SYMBOL_FLT;
        if (ui_reconfigure_output_for_input_policy(opts, state) == 0) {
            ui_set_toast(state, 3, "Applied: Symbol stream input -> %s", opts->audio_in_dev);
        } else {
            return UI_CMD_APPLY_FAILED;
        }
    }
    return UI_CMD_APPLY_COMPLETED;
}

static int
ui_cmd_handle_input_set_pulse(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "pulse");
    opts->audio_in_type = AUDIO_IN_PULSE;
    if (ui_reconfigure_output_for_input_policy(opts, state) == 0) {
        ui_set_toast(state, 3, "Applied: Input switched to Pulse");
    } else {
        return UI_CMD_APPLY_FAILED;
    }
    return UI_CMD_APPLY_COMPLETED;
}

static int
apply_cmd_io_and_import_file_outputs_b(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    static const struct dsd_app_command_handler_entry k_handlers[] = {
        {DSD_APP_CMD_SYMCAP_OPEN, ui_cmd_handle_symcap_open},
        {DSD_APP_CMD_SYMBOL_IN_OPEN, ui_cmd_handle_symbol_in_open},
        {DSD_APP_CMD_INPUT_WAV_SET, ui_cmd_handle_input_wav_set},
        {DSD_APP_CMD_INPUT_SYM_STREAM_SET, ui_cmd_handle_input_sym_stream_set},
        {DSD_APP_CMD_INPUT_SET_PULSE, ui_cmd_handle_input_set_pulse},
    };
    if (!opts || !c) {
        return 0;
    }
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

static int
ui_cmd_parse_host_port_payload(const struct dsd_app_command* c, char* host, size_t host_sz, int32_t* port) {
    if (!c || !host || host_sz == 0 || !port || c->n < (int)(256 + sizeof(int32_t))) {
        return 0;
    }
    DSD_MEMSET(host, 0, host_sz);
    DSD_MEMCPY(host, c->data, (host_sz > 255) ? 255 : (host_sz - 1));
    DSD_MEMCPY(port, c->data + 256, sizeof *port);
    return 1;
}

static int
ui_cmd_handle_udp_out_cfg(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    char host[256] = {0};
    int32_t port = 0;
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && ui_cmd_parse_host_port_payload(c, host, sizeof host, &port)) {
        int rc = svc_udp_output_config(opts, state, host, port);
        result = ui_cmd_apply_status_from_service_rc(rc);
        if (rc == 0) {
            ui_set_toast(state, 3, "UDP output configured: %s:%d", host, (int)port);
        } else {
            ui_set_toast(state, 4, "UDP output failed: %s:%d", host, (int)port);
        }
    }
    return result;
}

static int
ui_cmd_handle_tcp_connect_audio_cfg(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    char host[256] = {0};
    int32_t port = 0;
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && ui_cmd_parse_host_port_payload(c, host, sizeof host, &port)) {
        int rc = svc_tcp_connect_audio(opts, host, port);
        result = ui_cmd_apply_status_from_service_rc(rc);
        if (rc == 0) {
            ui_set_tcp_audio_connected_toast_if_output_ready(opts, state, host, (int)port);
        } else {
            ui_set_toast(state, 4, "TCP audio connect failed: %s:%d", host, (int)port);
        }
    }
    return result;
}

static int
ui_cmd_handle_rigctl_connect_cfg(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    char host[256] = {0};
    int32_t port = 0;
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && ui_cmd_parse_host_port_payload(c, host, sizeof host, &port)) {
        int rc = svc_rigctl_connect(opts, host, port);
        result = ui_cmd_apply_status_from_service_rc(rc);
        if (rc == 0) {
            ui_set_toast(state, 3, "Rigctl connected: %s:%d", host, (int)port);
        } else {
            ui_set_toast(state, 4, "Rigctl connect failed: %s:%d", host, (int)port);
        }
    }
    return result;
}

static int
ui_cmd_handle_udp_input_cfg(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    char bind[256] = {0};
    int32_t port = 0;
    if (state && ui_cmd_parse_host_port_payload(c, bind, sizeof bind, &port)) {
        DSD_SNPRINTF(opts->udp_in_bindaddr, sizeof opts->udp_in_bindaddr, "%s", bind);
        opts->udp_in_portno = port;
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "udp");
        opts->audio_in_type = AUDIO_IN_UDP;
        if (ui_reconfigure_output_for_input_policy(opts, state) == 0) {
            ui_set_toast(state, 3, "UDP input set: %s:%d", bind[0] ? bind : "127.0.0.1", (int)port);
        } else {
            return UI_CMD_APPLY_FAILED;
        }
    }
    return UI_CMD_APPLY_COMPLETED;
}

static int
apply_cmd_io_and_import_network(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    static const struct dsd_app_command_handler_entry k_handlers[] = {
        {DSD_APP_CMD_UDP_OUT_CFG, ui_cmd_handle_udp_out_cfg},
        {DSD_APP_CMD_TCP_CONNECT_AUDIO_CFG, ui_cmd_handle_tcp_connect_audio_cfg},
        {DSD_APP_CMD_RIGCTL_CONNECT_CFG, ui_cmd_handle_rigctl_connect_cfg},
        {DSD_APP_CMD_UDP_INPUT_CFG, ui_cmd_handle_udp_input_cfg},
    };
    if (!opts || !c) {
        return 0;
    }
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

static int
ui_cmd_parse_i32_payload(const struct dsd_app_command* c, int32_t* out) {
    if (!c || !out || c->n < (int)sizeof(*out)) {
        return 0;
    }
    DSD_MEMCPY(out, c->data, sizeof *out);
    return 1;
}

static int
ui_cmd_parse_u32_payload(const struct dsd_app_command* c, uint32_t* out) {
    if (!c || !out || c->n < (int)sizeof(*out)) {
        return 0;
    }
    DSD_MEMCPY(out, c->data, sizeof *out);
    return 1;
}

static int
ui_cmd_parse_double_payload(const struct dsd_app_command* c, double* out) {
    if (!c || !out || c->n < (int)sizeof(*out)) {
        return 0;
    }
    DSD_MEMCPY(out, c->data, sizeof *out);
    return 1;
}

#ifdef USE_RADIO
static int
ui_cmd_handle_rtl_enable_input(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    int result = UI_CMD_APPLY_COMPLETED;
    if (state) {
        int rc = svc_rtl_enable_input(opts, state);
        result = ui_cmd_apply_status_from_service_rc(rc);
        if (rc == 0) {
            if (ui_reconfigure_output_for_input_policy(opts, state) == 0) {
                ui_set_toast(state, 3, "Applied: RTL input enabled");
            } else {
                result = UI_CMD_APPLY_FAILED;
            }
        } else if (ui_rc_is_not_supported(rc)) {
            ui_set_toast(state, 3, "Unsupported: active radio backend cannot enable RTL input");
        } else {
            ui_set_toast(state, 4, "Failed: RTL input enable");
        }
    }
    return result;
}

static int
ui_cmd_handle_rtl_restart(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    int result = UI_CMD_APPLY_COMPLETED;
    if (state) {
        int rc = svc_rtl_restart(opts, state);
        result = ui_cmd_apply_status_from_service_rc(rc);
        if (rc == 0) {
            ui_set_toast(state, 3, "Applied: RTL stream restarted");
        } else if (ui_rc_is_not_supported(rc)) {
            ui_set_toast(state, 3, "Unsupported: active radio backend cannot restart stream");
        } else {
            ui_set_toast(state, 4, "Failed: RTL stream restart");
        }
    }
    return result;
}

static int
ui_cmd_handle_rtl_set_dev(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int32_t v = 0;
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && ui_cmd_parse_i32_payload(c, &v)) {
        int rc = svc_rtl_set_dev_index(opts, state, v);
        result = ui_cmd_apply_status_from_service_rc(rc);
        if (rc == 0) {
            ui_set_toast(state, 3, "Applied: RTL device index -> %d", (int)v);
        } else if (ui_rc_is_not_supported(rc)) {
            ui_set_toast(state, 3, "Unsupported: device index is not available for current radio source");
        } else {
            ui_set_toast(state, 4, "Failed: RTL device index -> %d", (int)v);
        }
    }
    return result;
}

static int
apply_cmd_io_and_import_rtl_a(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    static const struct dsd_app_command_handler_entry k_handlers[] = {
        {DSD_APP_CMD_RTL_ENABLE_INPUT, ui_cmd_handle_rtl_enable_input},
        {DSD_APP_CMD_RTL_RESTART, ui_cmd_handle_rtl_restart},
        {DSD_APP_CMD_RTL_SET_DEV, ui_cmd_handle_rtl_set_dev},
    };
    if (!opts || !c) {
        return 0;
    }
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

static int
ui_cmd_handle_rtl_set_freq(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    uint32_t v = 0;
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && ui_cmd_parse_u32_payload(c, &v)) {
        int rc = svc_rtl_set_freq(opts, state, v);
        result = ui_cmd_apply_status_from_tune_rc(rc);
        if (rc == 0) {
            ui_set_toast(state, 3, "Applied: RTL frequency -> %u Hz", v);
        } else if (rc == RTL_STREAM_TUNE_TIMEOUT) {
            ui_set_toast(state, 3, "Accepted: RTL frequency -> %u Hz (pending)", v);
        } else if (ui_rc_is_not_supported(rc)) {
            ui_set_toast(state, 3, "Unsupported: frequency control not available on active backend");
        } else {
            ui_set_toast(state, 4, "Failed: RTL frequency -> %u Hz", v);
        }
    }
    return result;
}

static int
ui_cmd_handle_rtl_set_gain(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int32_t v = 0;
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && ui_cmd_parse_i32_payload(c, &v)) {
        int rc = svc_rtl_set_gain(opts, state, v);
        result = ui_cmd_apply_status_from_service_rc(rc);
        if (rc == 0) {
            ui_set_toast(state, 3, "Applied: RTL gain -> %d", opts->rtl_gain_value);
        } else if (ui_rc_is_not_supported(rc)) {
            ui_set_toast(state, 3, "Unsupported: gain control not available on active backend");
        } else {
            ui_set_toast(state, 4, "Failed: RTL gain -> %d", (int)v);
        }
    }
    return result;
}

static int
ui_cmd_handle_rtl_set_ppm(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int32_t v = 0;
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && ui_cmd_parse_i32_payload(c, &v)) {
        int rc = rtl_stream_request_ppm(opts, v);
        result = ui_cmd_apply_status_from_service_rc(rc);
        if (rc == 0) {
            ui_set_toast(state, 3, "Applied: RTL PPM -> %d", rtl_stream_get_requested_ppm(opts));
        } else if (ui_rc_is_not_supported(rc)) {
            ui_set_toast(state, 3, "Unsupported: PPM correction not available on active backend");
        } else {
            ui_set_toast(state, 4, "Failed: RTL PPM update");
        }
    }
    return result;
}

static int
apply_cmd_io_and_import_rtl_b(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    static const struct dsd_app_command_handler_entry k_handlers[] = {
        {DSD_APP_CMD_RTL_SET_FREQ, ui_cmd_handle_rtl_set_freq},
        {DSD_APP_CMD_RTL_SET_GAIN, ui_cmd_handle_rtl_set_gain},
        {DSD_APP_CMD_RTL_SET_PPM, ui_cmd_handle_rtl_set_ppm},
    };
    if (!opts || !c) {
        return 0;
    }
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

static int
ui_cmd_handle_rtl_set_bw(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int32_t v = 0;
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && ui_cmd_parse_i32_payload(c, &v)) {
        int rc = svc_rtl_set_bandwidth(opts, state, v);
        result = ui_cmd_apply_status_from_service_rc(rc);
        if (rc == 0) {
            ui_set_toast(state, 3, "Applied: RTL DSP BW -> %d kHz", (int)opts->rtl_dsp_bw_khz);
        } else if (ui_rc_is_not_supported(rc)) {
            ui_set_toast(state, 3, "Unsupported: bandwidth control not available on active backend");
        } else {
            ui_set_toast(state, 4, "Failed: RTL DSP BW update");
        }
    }
    return result;
}

static int
ui_cmd_handle_rtl_set_sql_db(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    double d = 0.0;
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && ui_cmd_parse_double_payload(c, &d)) {
        int rc = svc_rtl_set_sql_db(opts, d);
        result = ui_cmd_apply_status_from_service_rc(rc);
        if (rc == 0) {
            ui_set_toast(state, 3, "Applied: RTL squelch -> %.1f dB", d);
        } else if (ui_rc_is_not_supported(rc)) {
            ui_set_toast(state, 3, "Unsupported: squelch control not available on active backend");
        } else {
            ui_set_toast(state, 4, "Failed: RTL squelch update");
        }
    }
    return result;
}

static int
ui_cmd_handle_rtl_set_vol_mult(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int32_t v = 0;
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && ui_cmd_parse_i32_payload(c, &v)) {
        int rc = svc_rtl_set_volume_mult(opts, v);
        result = ui_cmd_apply_status_from_service_rc(rc);
        if (rc == 0) {
            ui_set_toast(state, 3, "Applied: RTL monitor gain -> %dX", (int)opts->rtl_volume_multiplier);
        } else if (ui_rc_is_not_supported(rc)) {
            ui_set_toast(state, 3, "Unsupported: monitor gain not available on active backend");
        } else {
            ui_set_toast(state, 4, "Failed: RTL monitor gain update");
        }
    }
    return result;
}

static int
apply_cmd_io_and_import_rtl_c(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    static const struct dsd_app_command_handler_entry k_handlers[] = {
        {DSD_APP_CMD_RTL_SET_BW, ui_cmd_handle_rtl_set_bw},
        {DSD_APP_CMD_RTL_SET_SQL_DB, ui_cmd_handle_rtl_set_sql_db},
        {DSD_APP_CMD_RTL_SET_VOL_MULT, ui_cmd_handle_rtl_set_vol_mult},
    };
    if (!opts || !c) {
        return 0;
    }
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

static int
ui_cmd_handle_rtl_set_bias_tee(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int32_t on = 0;
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && ui_cmd_parse_i32_payload(c, &on)) {
        int rc = svc_rtl_set_bias_tee(opts, state, on);
        result = ui_cmd_apply_status_from_service_rc(rc);
        if (rc == 0) {
            ui_set_toast(state, 3, "Applied: RTL bias tee -> %s", on ? "On" : "Off");
        } else if (ui_rc_is_not_supported(rc)) {
            ui_set_toast(state, 3, "Unsupported: bias tee control not available on active backend");
        } else {
            ui_set_toast(state, 4, "Failed: RTL bias tee update");
        }
    }
    return result;
}

static int
ui_cmd_handle_rtltcp_set_autotune(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int32_t on = 0;
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && ui_cmd_parse_i32_payload(c, &on)) {
        int rc = svc_rtltcp_set_autotune(opts, state, on);
        result = ui_cmd_apply_status_from_service_rc(rc);
        if (rc == 0) {
            ui_set_toast(state, 3, "Applied: RTL-TCP adaptive networking -> %s", on ? "On" : "Off");
        } else if (ui_rc_is_not_supported(rc)) {
            ui_set_toast(state, 3, "Unsupported: RTL-TCP autotune not available on active backend");
        } else {
            ui_set_toast(state, 4, "Failed: RTL-TCP adaptive networking update");
        }
    }
    return result;
}

static int
ui_cmd_handle_rtl_set_auto_ppm(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int32_t on = 0;
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && ui_cmd_parse_i32_payload(c, &on)) {
        int rc = svc_rtl_set_auto_ppm(opts, state, on);
        result = ui_cmd_apply_status_from_service_rc(rc);
        if (rc == 0) {
            ui_set_toast(state, 3, "Applied: Auto PPM -> %s", on ? "On" : "Off");
        } else if (ui_rc_is_not_supported(rc)) {
            ui_set_toast(state, 3, "Unsupported: Auto PPM not available on active backend");
        } else {
            ui_set_toast(state, 4, "Failed: Auto PPM update");
        }
    }
    return result;
}

static int
apply_cmd_io_and_import_rtl_d(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    static const struct dsd_app_command_handler_entry k_handlers[] = {
        {DSD_APP_CMD_RTL_SET_BIAS_TEE, ui_cmd_handle_rtl_set_bias_tee},
        {DSD_APP_CMD_RTLTCP_SET_AUTOTUNE, ui_cmd_handle_rtltcp_set_autotune},
        {DSD_APP_CMD_RTL_SET_AUTO_PPM, ui_cmd_handle_rtl_set_auto_ppm},
    };
    if (!opts || !c) {
        return 0;
    }
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}
#endif

static int
ui_cmd_handle_rigctl_set_mod_bw(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int32_t hz = 0;
    if (state && ui_cmd_parse_i32_payload(c, &hz)) {
        svc_set_rigctl_setmod_bw(opts, hz);
        ui_set_toast(state, 3, "Applied: Rigctl setmod BW -> %d Hz", opts->setmod_bw);
    }
    return 1;
}

static int
ui_cmd_handle_tg_hold_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    uint32_t tg = 0;
    (void)opts;
    if (state && ui_cmd_parse_u32_payload(c, &tg)) {
        svc_set_tg_hold(state, tg);
        ui_set_toast(state, 3, "Applied: TG Hold -> %u", tg);
    }
    return 1;
}

static int
ui_cmd_handle_hangtime_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    double s = 0.0;
    if (state && ui_cmd_parse_double_payload(c, &s)) {
        svc_set_hangtime(opts, s);
        ui_set_toast(state, 3, "Applied: Hangtime -> %.3f s", opts->trunk_hangtime);
    }
    return 1;
}

static int
ui_cmd_handle_slot_pref_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int32_t p = 0;
    if (state && ui_cmd_parse_i32_payload(c, &p)) {
        svc_set_slot_pref(opts, p);
        const char* label = (opts->slot_preference == 0) ? "Slot 1" : (opts->slot_preference == 1) ? "Slot 2" : "Auto";
        ui_set_toast(state, 3, "Applied: Slot preference -> %s", label);
    }
    return 1;
}

static int
ui_cmd_handle_slots_onoff_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int32_t m = 0;
    if (state && ui_cmd_parse_i32_payload(c, &m)) {
        svc_set_slots_onoff(opts, m);
        ui_set_toast(state, 3, "Applied: Slot mask -> %d", (opts->slot1_on ? 1 : 0) | (opts->slot2_on ? 2 : 0));
    }
    return 1;
}

static int
apply_cmd_io_and_import_runtime_a(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    static const struct dsd_app_command_handler_entry k_handlers[] = {
        {DSD_APP_CMD_RIGCTL_SET_MOD_BW, ui_cmd_handle_rigctl_set_mod_bw},
        {DSD_APP_CMD_TG_HOLD_SET, ui_cmd_handle_tg_hold_set},
        {DSD_APP_CMD_HANGTIME_SET, ui_cmd_handle_hangtime_set},
        {DSD_APP_CMD_SLOT_PREF_SET, ui_cmd_handle_slot_pref_set},
        {DSD_APP_CMD_SLOTS_ONOFF_SET, ui_cmd_handle_slots_onoff_set},
    };
    if (!opts || !c) {
        return 0;
    }
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

static int
apply_cmd_io_and_import_pulse_io(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    if (!opts || !c) {
        return 0;
    }
    switch (c->id) {
        case DSD_APP_CMD_PULSE_OUT_SET: {
            int result = UI_CMD_APPLY_COMPLETED;
            if (state && c->n > 0) {
                char name[256] = {0};
                size_t n = c->n < sizeof(name) ? c->n : sizeof(name) - 1;
                DSD_MEMCPY(name, c->data, n);
                name[n] = '\0';
                int rc = svc_set_pulse_output(opts, name);
                result = ui_cmd_apply_status_from_service_rc(rc);
                if (rc == 0) {
                    ui_set_toast(state, 3, "Applied: Pulse output -> %s", name);
                } else {
                    ui_set_toast(state, 4, "Failed: Pulse output -> %s", name);
                }
            }
            return result;
        }
        case DSD_APP_CMD_PULSE_IN_SET: {
            int result = UI_CMD_APPLY_COMPLETED;
            if (state && c->n > 0) {
                char name[256] = {0};
                size_t n = c->n < sizeof(name) ? c->n : sizeof(name) - 1;
                DSD_MEMCPY(name, c->data, n);
                name[n] = '\0';
                int rc = svc_set_pulse_input(opts, name);
                result = ui_cmd_apply_status_from_service_rc(rc);
                if (rc == 0) {
                    if (ui_reconfigure_output_for_input_policy(opts, state) == 0) {
                        ui_set_toast(state, 3, "Applied: Pulse input -> %s", name);
                    } else {
                        result = UI_CMD_APPLY_FAILED;
                    }
                } else {
                    ui_set_toast(state, 4, "Failed: Pulse input -> %s", name);
                }
            }
            return result;
        }
        default: return 0;
    }
}

static int
ui_cmd_handle_lrrp_set_home(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    int result = UI_CMD_APPLY_COMPLETED;
    if (state) {
        int rc = svc_lrrp_set_home(opts);
        result = ui_cmd_apply_status_from_service_rc(rc);
        if (rc == 0) {
            ui_set_toast(state, 3, "Applied: LRRP output -> %s", opts->lrrp_out_file);
        } else {
            ui_set_toast(state, 4, "Failed: LRRP output (home)");
        }
    }
    return result;
}

static int
ui_cmd_handle_lrrp_set_dsdp(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    int result = UI_CMD_APPLY_COMPLETED;
    if (state) {
        int rc = svc_lrrp_set_dsdp(opts);
        result = ui_cmd_apply_status_from_service_rc(rc);
        if (rc == 0) {
            ui_set_toast(state, 3, "Applied: LRRP output -> %s", opts->lrrp_out_file);
        } else {
            ui_set_toast(state, 4, "Failed: LRRP output (DSDPlus)");
        }
    }
    return result;
}

static int
ui_cmd_handle_lrrp_set_custom(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && c->n > 0) {
        char path[1024] = {0};
        if (ui_cmd_copy_payload_string(c, path, sizeof path)) {
            int rc = svc_lrrp_set_custom(opts, path);
            result = ui_cmd_apply_status_from_service_rc(rc);
            if (rc == 0) {
                ui_set_toast(state, 3, "Applied: LRRP output -> %s", path);
            } else {
                ui_set_toast(state, 4, "Failed: LRRP output -> %s", path);
            }
        }
    }
    return result;
}

static int
ui_cmd_handle_lrrp_disable(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    if (state) {
        svc_lrrp_disable(opts);
        ui_set_toast(state, 3, "Applied: LRRP output disabled");
    }
    return 1;
}

static int
ui_cmd_handle_p25_p2_params_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    struct {
        uint64_t w;
        uint64_t s;
        uint64_t n;
    } p = {0};

    (void)opts;
    if (state && c->n >= (int)sizeof p) {
        DSD_MEMCPY(&p, c->data, sizeof p);
        svc_set_p2_params(state, p.w, p.s, p.n);
        ui_set_toast(state, 3, "Applied: P25 P2 params W:%llX S:%llX N:%llX", (unsigned long long)state->p2_wacn,
                     (unsigned long long)state->p2_sysid, (unsigned long long)state->p2_cc);
    }
    return 1;
}

static int
apply_cmd_io_and_import_lrrp_and_p2(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    static const struct dsd_app_command_handler_entry k_handlers[] = {
        {DSD_APP_CMD_LRRP_SET_HOME, ui_cmd_handle_lrrp_set_home},
        {DSD_APP_CMD_LRRP_SET_DSDP, ui_cmd_handle_lrrp_set_dsdp},
        {DSD_APP_CMD_LRRP_SET_CUSTOM, ui_cmd_handle_lrrp_set_custom},
        {DSD_APP_CMD_LRRP_DISABLE, ui_cmd_handle_lrrp_disable},
        {DSD_APP_CMD_P25_P2_PARAMS_SET, ui_cmd_handle_p25_p2_params_set},
    };
    if (!opts || !c) {
        return 0;
    }
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

static int
ui_cmd_handle_import_channel_map(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && c->n > 0) {
        char path[1024] = {0};
        if (ui_cmd_copy_payload_string(c, path, sizeof path)) {
            int rc = svc_import_channel_map(opts, state, path);
            result = ui_cmd_apply_status_from_service_rc(rc);
            if (rc == 0) {
                ui_set_toast(state, 3, "Applied: Channel map imported -> %s", path);
            } else {
                ui_set_toast(state, 4, "Failed: Channel map import -> %s", path);
            }
        }
    }
    return result;
}

static int
ui_cmd_handle_import_group_list(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && c->n > 0) {
        char path[1024] = {0};
        if (ui_cmd_copy_payload_string(c, path, sizeof path)) {
            int rc = svc_import_group_list(opts, state, path);
            result = ui_cmd_apply_status_from_service_rc(rc);
            if (rc == 0) {
                ui_set_toast(state, 3, "Applied: Group list reloaded -> %s", path);
            } else {
                ui_set_toast(state, 4, "Failed: Group list reload -> %s", path);
            }
        }
    }
    return result;
}

static int
ui_cmd_handle_import_keys_dec(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && c->n > 0) {
        char path[1024] = {0};
        if (ui_cmd_copy_payload_string(c, path, sizeof path)) {
            int rc = svc_import_keys_dec(opts, state, path);
            result = ui_cmd_apply_status_from_service_rc(rc);
            if (rc == 0) {
                p25_sm_clear_encrypted_call_cache(state);
                ui_set_toast(state, 3, "Applied: Keys (DEC) imported -> %s", path);
            } else {
                ui_set_toast(state, 4, "Failed: Keys (DEC) import -> %s", path);
            }
        }
    }
    return result;
}

static int
ui_cmd_handle_import_keys_hex(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && c->n > 0) {
        char path[1024] = {0};
        if (ui_cmd_copy_payload_string(c, path, sizeof path)) {
            int rc = svc_import_keys_hex(opts, state, path);
            result = ui_cmd_apply_status_from_service_rc(rc);
            if (rc == 0) {
                p25_sm_clear_encrypted_call_cache(state);
                ui_set_toast(state, 3, "Applied: Keys (HEX) imported -> %s", path);
            } else {
                ui_set_toast(state, 4, "Failed: Keys (HEX) import -> %s", path);
            }
        }
    }
    return result;
}

static int
apply_cmd_io_and_import_imports(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    static const struct dsd_app_command_handler_entry k_handlers[] = {
        {DSD_APP_CMD_IMPORT_CHANNEL_MAP, ui_cmd_handle_import_channel_map},
        {DSD_APP_CMD_IMPORT_GROUP_LIST, ui_cmd_handle_import_group_list},
        {DSD_APP_CMD_IMPORT_KEYS_DEC, ui_cmd_handle_import_keys_dec},
        {DSD_APP_CMD_IMPORT_KEYS_HEX, ui_cmd_handle_import_keys_hex},
    };
    if (!opts || !c) {
        return 0;
    }
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

static int
apply_cmd_io_and_import(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    if (!opts || !c) {
        return 0;
    }
    int r = apply_cmd_io_and_import_file_outputs_a(opts, state, c);
    if (r) {
        return r;
    }
    r = apply_cmd_io_and_import_file_outputs_b(opts, state, c);
    if (r) {
        return r;
    }
    r = apply_cmd_io_and_import_network(opts, state, c);
    if (r) {
        return r;
    }
#ifdef USE_RADIO
    r = apply_cmd_io_and_import_rtl_a(opts, state, c);
    if (r) {
        return r;
    }
    r = apply_cmd_io_and_import_rtl_b(opts, state, c);
    if (r) {
        return r;
    }
    r = apply_cmd_io_and_import_rtl_c(opts, state, c);
    if (r) {
        return r;
    }
    r = apply_cmd_io_and_import_rtl_d(opts, state, c);
    if (r) {
        return r;
    }
#endif
    r = apply_cmd_io_and_import_runtime_a(opts, state, c);
    if (r) {
        return r;
    }
    r = apply_cmd_io_and_import_pulse_io(opts, state, c);
    if (r) {
        return r;
    }
    r = apply_cmd_io_and_import_lrrp_and_p2(opts, state, c);
    if (r) {
        return r;
    }
    r = apply_cmd_io_and_import_imports(opts, state, c);
    if (r) {
        return r;
    }
    return 0;
}

#ifdef USE_RADIO
static int
apply_cmd_dsp(const struct dsd_app_command* c) {
    if (!c || c->id != DSD_APP_CMD_DSP_OP) {
        return 0;
    }
    dsd_app_dsp_payload p = {0};
    if (c->n >= (int)sizeof(dsd_app_dsp_payload)) {
        DSD_MEMCPY(&p, c->data, sizeof p);
    }
    apply_dsp_op(&p);
    return 1;
}
#endif

static long
current_cc_freq(const dsd_state* state) {
    return (state->trunk_cc_freq != 0) ? state->trunk_cc_freq : state->p25_cc_freq;
}

static void
reset_call_tracking(dsd_opts* opts, dsd_state* state, int clear_trunk_vc) {
    const double ended_m = dsd_time_now_monotonic_s();
    for (int slot = 0; slot < DSD_CALL_STATE_SLOT_COUNT; slot++) {
        if (dsd_call_state_end(state, (uint8_t)slot, ended_m) > 0) {
            dsd_event_sync_slot(opts, state, (uint8_t)slot);
        }
    }
    DSD_MEMSET(state->nxdn_sacch_frame_segment, 1, sizeof(state->nxdn_sacch_frame_segment));
    DSD_MEMSET(state->nxdn_sacch_frame_segcrc, 1, sizeof(state->nxdn_sacch_frame_segcrc));
    (void)dsd_recent_activity_clear_all(state);
    dmr_reset_blocks(opts, state);
    state->lasttg = state->lasttgR = 0;
    state->lastsrc = state->lastsrcR = 0;
    state->payload_algid = state->payload_algidR = 0;
    state->payload_keyid = state->payload_keyidR = 0;
    state->payload_mi = state->payload_miR = state->payload_miP = state->payload_miN = 0;
    opts->trunk_is_tuned = 0;
    state->p25_vc_freq[0] = state->p25_vc_freq[1] = 0;
    if (clear_trunk_vc) {
        state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
    }
}

static int
current_demod_rate(const dsd_opts* opts, const dsd_state* state) {
#ifdef USE_RADIO
    if (opts->audio_in_type == AUDIO_IN_RTL && state->rtl_ctx) {
        int demod_rate = (int)rtl_stream_output_rate(state->rtl_ctx);
        if (demod_rate > 0) {
            return demod_rate;
        }
    }
#else
    (void)state;
#endif
    return dsd_opts_current_input_timing_rate(opts);
}

static int
cc_has_active_p25_context(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state || (opts->frame_p25p1 != 1 && opts->frame_p25p2 != 1)) {
        return 0;
    }
    if (state->synctype != DSD_SYNC_NONE) {
        return DSD_SYNC_IS_P25(state->synctype) ? 1 : 0;
    }
    if (state->lastsynctype != DSD_SYNC_NONE) {
        return DSD_SYNC_IS_P25(state->lastsynctype) ? 1 : 0;
    }
    /* A P25 voice tune remains authoritative while frame sync is temporarily absent. */
    return opts->trunk_is_tuned == 1 && (state->p25_vc_freq[0] != 0 || state->p25_vc_freq[1] != 0);
}

static int
cc_symbol_rate(const dsd_opts* opts, const dsd_state* state, int fdma_only) {
    if (!cc_has_active_p25_context(opts, state)) {
        return 0;
    }
    if (state->p25_cc_is_tdma == 0) {
        return 4800;
    }
    if (!fdma_only && state->p25_cc_is_tdma == 1) {
        return 6000;
    }
    // Keep the retune profile-neutral unless the P25 CC type is known and allowed by the action.
    return 0;
}

static void
set_cc_symbol_timing(const dsd_opts* opts, dsd_state* state, int sym_rate) {
    if (sym_rate == 0) {
        return;
    }
    state->samplesPerSymbol = dsd_opts_compute_sps_rate(opts, sym_rate, current_demod_rate(opts, state));
    state->symbolCenter = dsd_opts_symbol_center(state->samplesPerSymbol);
    state->sps_hunt_idx = sym_rate == 6000 ? DSD_FRAME_SYNC_SPS_PROFILE_6000_4 : DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state->sps_hunt_counter = 0;
}

static int
request_manual_tune(dsd_opts* opts, dsd_state* state, long int freq, int p25_cc_symbol_rate, const char* action) {
    int result = 0;
    int accepted = 0;
    if (p25_cc_symbol_rate != 0) {
        const int ted_sps = dsd_opts_compute_sps_rate(opts, p25_cc_symbol_rate, current_demod_rate(opts, state));
        const dsd_trunk_tune_result cc_result = dsd_trunk_tuning_hook_tune_to_cc(opts, state, freq, ted_sps, NULL);
        result = (int)cc_result;
        accepted = dsd_trunk_tune_result_is_ok(cc_result);
    } else {
        /* Generic LCN actions and unknown CC types must not stage a P25 RTL profile. */
        result = io_control_set_freq(opts, state, freq);
        accepted = result == RTL_STREAM_TUNE_OK || result == RTL_STREAM_TUNE_TIMEOUT;
    }
    if (accepted) {
        return 1;
    }
    LOG_WARN("WARNING: %s tune to %ld Hz was not accepted (result=%d); preserving decoder state\n",
             action ? action : "Manual", freq, result);
    return 0;
}

static void
mark_cc_sync(dsd_state* state, int include_monotonic) {
    state->last_cc_sync_time = time(NULL);
    if (include_monotonic) {
        state->last_cc_sync_time_m = dsd_time_now_monotonic_s();
    }
}

static int
apply_manual_return_to_cc(dsd_opts* opts, dsd_state* state) {
    if (!state) {
        return UI_CMD_APPLY_COMPLETED;
    }
    if (opts->trunk_enable != 1 || (state->trunk_cc_freq == 0 && state->p25_cc_freq == 0)) {
        return UI_CMD_APPLY_COMPLETED;
    }

    const long freq = current_cc_freq(state);
    const int sym_rate = cc_symbol_rate(opts, state, 0);
    if (!request_manual_tune(opts, state, freq, sym_rate, "Return-to-CC")) {
        return UI_CMD_APPLY_FAILED;
    }
    reset_call_tracking(opts, state, 1);
    mark_cc_sync(state, 1);
    set_cc_symbol_timing(opts, state, sym_rate);
    LOG_INFO("User Activated Return to CC\n");
    return UI_CMD_APPLY_COMPLETED;
}

static int
apply_lockout_decoder_transition(dsd_opts* opts, dsd_state* state) {
    long cc_freq = 0;
    const int sym_rate = cc_symbol_rate(opts, state, 1);
    if (opts->trunk_enable == 1) {
        cc_freq = current_cc_freq(state);
        if (cc_freq != 0 && !request_manual_tune(opts, state, cc_freq, sym_rate, "Lockout return-to-CC")) {
            return UI_CMD_APPLY_FAILED;
        }
    }

    reset_call_tracking(opts, state, 1);
    if (opts->trunk_enable == 1) {
        noCarrier(opts, state);
        state->trunk_cc_freq = cc_freq;
    }
    mark_cc_sync(state, 0);
    set_cc_symbol_timing(opts, state, sym_rate);
    return UI_CMD_APPLY_COMPLETED;
}

static int
try_manual_candidate_cycle(dsd_opts* opts, dsd_state* state) {
    long cand = 0;
    if (opts->p25_prefer_candidates != 1 || !p25_cc_next_candidate(state, &cand)) {
        return UI_CMD_APPLY_UNHANDLED;
    }
    const int sym_rate = cc_symbol_rate(opts, state, 0);
    if (!request_manual_tune(opts, state, cand, sym_rate, "Candidate cycle")) {
        return UI_CMD_APPLY_FAILED;
    }

    reset_call_tracking(opts, state, 0);
    LOG_INFO("Candidate Cycle: tuning to %.06lf MHz\n", (double)cand / 1000000);
    mark_cc_sync(state, 1);
    set_cc_symbol_timing(opts, state, sym_rate);
    return UI_CMD_APPLY_COMPLETED;
}

static int
apply_manual_lcn_cycle(dsd_opts* opts, dsd_state* state) {
    const int lcn_capacity = (int)(sizeof(state->trunk_lcn_freq) / sizeof(state->trunk_lcn_freq[0]));
    int count = state->lcn_freq_count;
    if (count <= 0) {
        return UI_CMD_APPLY_COMPLETED;
    }
    if (count > lcn_capacity) {
        count = lcn_capacity;
    }

    int next = state->lcn_freq_roll;
    if (next < 0 || next >= count) {
        next = 0;
    }
    const int start = next;
    long freq = 0;
    for (int examined = 0; examined < count; examined++) {
        freq = state->trunk_lcn_freq[next];
        if (freq != 0 && (next == 0 || state->trunk_lcn_freq[next - 1] != freq)) {
            break;
        }
        next++;
        if (next >= count) {
            next = 0;
        }
        freq = 0;
    }

    if (freq == 0) {
        state->lcn_freq_roll = start + 1;
        if (state->lcn_freq_roll >= count) {
            state->lcn_freq_roll = 0;
        }
        return UI_CMD_APPLY_COMPLETED;
    }
    if (!request_manual_tune(opts, state, freq, 0, "Channel cycle")) {
        return UI_CMD_APPLY_FAILED;
    }

    reset_call_tracking(opts, state, 0);
    LOG_INFO("Channel Cycle: tuning to %.06lf MHz\n", (double)freq / 1000000);
    state->lcn_freq_roll = next + 1;
    mark_cc_sync(state, 1);
    return UI_CMD_APPLY_COMPLETED;
}

static int
apply_manual_channel_cycle(dsd_opts* opts, dsd_state* state) {
    const int candidate_status = try_manual_candidate_cycle(opts, state);
    if (candidate_status != UI_CMD_APPLY_UNHANDLED) {
        return candidate_status;
    }
    return apply_manual_lcn_cycle(opts, state);
}

#ifdef USE_RADIO
static int
cfg_uses_rtl_runtime(const dsdneoUserConfig* cfg) {
    if (!cfg || !cfg->has_input) {
        return 0;
    }
    return cfg->input_source == DSDCFG_INPUT_RTL || cfg->input_source == DSDCFG_INPUT_RTLTCP
           || cfg->input_source == DSDCFG_INPUT_SOAPY;
}

static void
apply_cfg_live_rtl_ppm_request(dsd_opts* opts, const dsdneoUserConfig* cfg, int old_audio_in_type) {
    if (!opts || old_audio_in_type != AUDIO_IN_RTL || opts->audio_in_type != AUDIO_IN_RTL || !cfg_uses_rtl_runtime(cfg)
        || !cfg->rtl_ppm_is_set) {
        return;
    }
    /* Config apply must mint a fresh request generation even when the input
     * device string is unchanged, otherwise same-value retries after a failed
     * apply are mistaken for stale state and never reach the controller.
     * Omitted rtl_ppm must preserve the live correction instead of clearing it
     * back to the config struct's zero-initialized default. */
    (void)rtl_stream_request_ppm(opts, cfg->rtl_ppm);
}

static void
apply_cfg_rtl_common(dsd_opts* opts, const dsdneoUserConfig* cfg) {
    if (cfg->rtl_freq[0]) {
        uint32_t hz = dsd_parse_freq_hz(cfg->rtl_freq);
        if (hz > 0) {
            opts->rtlsdr_center_freq = hz;
        }
    }
    if (cfg->rtl_bw_khz) {
        opts->rtl_dsp_bw_khz = cfg->rtl_bw_khz;
    }
    if (cfg->rtl_sql) {
        double sql = (double)cfg->rtl_sql;
        if (sql > 1.0) {
            sql /= (32768.0 * 32768.0);
        }
        opts->rtl_squelch_level = sql;
        rtl_stream_set_channel_squelch((float)sql);
    }
    if (cfg->rtl_gain) {
        opts->rtl_gain_value = cfg->rtl_gain;
    }
    if (cfg->rtl_volume) {
        opts->rtl_volume_multiplier = cfg->rtl_volume;
    }
}

static void
apply_cfg_rtl_hot_restart(dsd_opts* opts, dsd_state* state, const dsdneoUserConfig* cfg, const char* old_audio_in_dev,
                          int old_audio_in_type) {
    if (!cfg->has_input
        || (cfg->input_source != DSDCFG_INPUT_RTL && cfg->input_source != DSDCFG_INPUT_RTLTCP
            && cfg->input_source != DSDCFG_INPUT_SOAPY)
        || old_audio_in_type != AUDIO_IN_RTL || opts->audio_in_type != AUDIO_IN_RTL
        || strncmp(old_audio_in_dev, opts->audio_in_dev, sizeof opts->audio_in_dev) == 0) {
        return;
    }

    if (cfg->input_source == DSDCFG_INPUT_RTL) {
        if (cfg->rtl_device >= 0) {
            opts->rtl_dev_index = cfg->rtl_device;
        }
        apply_cfg_rtl_common(opts, cfg);
        opts->rtltcp_enabled = 0;
    } else if (cfg->input_source == DSDCFG_INPUT_RTLTCP) {
        if (cfg->rtltcp_host[0]) {
            DSD_SNPRINTF(opts->rtltcp_hostname, sizeof opts->rtltcp_hostname, "%s", cfg->rtltcp_host);
        }
        if (cfg->rtltcp_port) {
            opts->rtltcp_portno = cfg->rtltcp_port;
        }
        apply_cfg_rtl_common(opts, cfg);
        opts->rtltcp_enabled = 1;
    } else { // DSDCFG_INPUT_SOAPY
        apply_cfg_rtl_common(opts, cfg);
        opts->rtltcp_enabled = 0;
    }
    (void)svc_rtl_restart(opts, state);
}
#endif

static void
apply_cfg_tcp_hot_restart(dsd_opts* opts, const dsdneoUserConfig* cfg, const char* old_audio_in_dev,
                          int old_audio_in_type) {
    const char* host = NULL;
    int port = 0;

    if (!cfg->has_input || cfg->input_source != DSDCFG_INPUT_TCP || old_audio_in_type != AUDIO_IN_TCP
        || strncmp(old_audio_in_dev, "tcp", 3) != 0 || strncmp(opts->audio_in_dev, "tcp", 3) != 0
        || strncmp(old_audio_in_dev, opts->audio_in_dev, sizeof opts->audio_in_dev) == 0) {
        return;
    }

    host = cfg->tcp_host[0] ? cfg->tcp_host : opts->tcp_hostname;
    port = cfg->tcp_port ? cfg->tcp_port : opts->tcp_portno;
    if (svc_tcp_connect_audio(opts, host, port) != 0) {
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", old_audio_in_dev);
        LOG_ERROR("Config: failed to reconnect TCP audio %s:%d\n", host, port);
    }
}

static void
apply_cfg_udp_hot_restart(dsd_opts* opts, const dsdneoUserConfig* cfg, const char* old_audio_in_dev,
                          int old_audio_in_type) {
    if (!cfg->has_input || cfg->input_source != DSDCFG_INPUT_UDP || old_audio_in_type != AUDIO_IN_UDP
        || strncmp(old_audio_in_dev, "udp", 3) != 0 || strncmp(opts->audio_in_dev, "udp", 3) != 0
        || strncmp(old_audio_in_dev, opts->audio_in_dev, sizeof opts->audio_in_dev) == 0) {
        return;
    }

    if (cfg->udp_addr[0]) {
        DSD_SNPRINTF(opts->udp_in_bindaddr, sizeof opts->udp_in_bindaddr, "%s", cfg->udp_addr);
    }
    if (cfg->udp_port) {
        opts->udp_in_portno = cfg->udp_port;
    }
    if (opts->udp_in_ctx) {
        udp_input_stop(opts);
    }
    const char* bindaddr = opts->udp_in_bindaddr[0] ? opts->udp_in_bindaddr : "127.0.0.1";
    int port = opts->udp_in_portno ? opts->udp_in_portno : 7355;
    if (udp_input_start(opts, bindaddr, port, opts->wav_sample_rate) != 0) {
        LOG_ERROR("Config: failed to restart UDP input %s:%d\n", bindaddr, port);
    }
}

static void
rollback_cfg_file_hot_restart(dsd_opts* opts, dsd_state* state, const char* old_audio_in_dev, int old_audio_in_type,
                              int old_wav_sample_rate, int old_effective_input_rate, int failed_effective_input_rate) {
    if (!opts) {
        return;
    }

    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", old_audio_in_dev);
    opts->audio_in_type = old_audio_in_type;
    dsd_opts_apply_input_sample_rate(opts, old_wav_sample_rate);

    if (state && failed_effective_input_rate != old_effective_input_rate) {
        dsd_state_rescale_symbol_timing(state, failed_effective_input_rate, old_effective_input_rate);
    }
}

static void
restore_symbol_timing_snapshot(dsd_state* state, int old_samples_per_symbol, int old_symbol_center, int old_jitter) {
    if (!state) {
        return;
    }

    int restored_sps = old_samples_per_symbol > 0 ? old_samples_per_symbol : 10;
    int restored_center = old_symbol_center;
    if (restored_center < 0 || restored_center >= restored_sps) {
        restored_center = dsd_opts_symbol_center(restored_sps);
    }

    state->samplesPerSymbol = restored_sps;
    state->symbolCenter = restored_center;
    state->jitter = old_jitter;
}

static void
restore_live_pcm_rate_after_staged_file_apply(dsd_opts* opts, const dsdneoUserConfig* cfg, int old_wav_sample_rate) {
    if (!opts || !cfg || !cfg->has_input || cfg->input_source != DSDCFG_INPUT_FILE || !cfg->file_path[0]) {
        return;
    }
    if (opts->audio_in_type == AUDIO_IN_WAV) {
        return;
    }

    opts->staged_file_sample_rate = (cfg->file_sample_rate > 0) ? cfg->file_sample_rate : 48000;
    if (opts->wav_sample_rate != old_wav_sample_rate) {
        dsd_opts_apply_input_sample_rate(opts, old_wav_sample_rate);
    }
}

static int
audio_file_info_uses_container_metadata(const SF_INFO* info) {
    if (!info) {
        return 0;
    }

    int major_format = info->format & SF_FORMAT_TYPEMASK;
    return major_format != 0 && major_format != SF_FORMAT_RAW;
}

static int
cfg_file_runtime_should_apply(const dsd_opts* opts, const dsd_state* state, const dsdneoUserConfig* cfg) {
    return opts && state && cfg && cfg->has_input && cfg->input_source == DSDCFG_INPUT_FILE && cfg->file_path[0];
}

static int
cfg_file_runtime_configured_rate(const dsd_opts* opts) {
    int configured_effective_input_rate = dsd_opts_effective_input_rate(opts);
    if (configured_effective_input_rate <= 0) {
        configured_effective_input_rate = 48000;
    }
    return configured_effective_input_rate;
}

static void
cfg_file_runtime_apply_wav_input(dsd_opts* opts, dsd_state* state, const dsdneoUserConfig* cfg,
                                 int old_samples_per_symbol, int old_symbol_center, int old_jitter,
                                 int configured_effective_input_rate) {
    if (strncmp(opts->audio_in_dev, cfg->file_path, sizeof opts->audio_in_dev) != 0) {
        restore_symbol_timing_snapshot(state, old_samples_per_symbol, old_symbol_center, old_jitter);
        return;
    }

    int active_sample_rate = opts->wav_sample_rate;
    if (audio_file_info_uses_container_metadata(opts->audio_in_file_info) && opts->audio_in_file_info->samplerate > 0) {
        active_sample_rate = opts->audio_in_file_info->samplerate;
    }
    if (active_sample_rate <= 0) {
        active_sample_rate = 48000;
    }
    if (opts->wav_sample_rate != active_sample_rate) {
        dsd_opts_apply_input_sample_rate(opts, active_sample_rate);
    }

    int active_effective_input_rate = dsd_opts_effective_input_rate(opts);
    if (cfg->has_mode) {
        dsd_apply_decode_mode_symbol_timing(cfg->decode_mode, active_effective_input_rate, state);
        dsd_audio_rescale_symbol_timing(state, active_effective_input_rate, active_effective_input_rate);
        return;
    }
    dsd_audio_rescale_symbol_timing(state, configured_effective_input_rate, active_effective_input_rate);
}

static void
cfg_file_runtime_apply_live_input(const dsd_opts* opts, dsd_state* state, const dsdneoUserConfig* cfg,
                                  int old_runtime_input_rate, int old_samples_per_symbol, int old_symbol_center,
                                  int old_jitter) {
    if (old_runtime_input_rate <= 0) {
        old_runtime_input_rate = current_demod_rate(opts, state);
    }
    if (old_runtime_input_rate <= 0) {
        old_runtime_input_rate = 48000;
    }

    if (cfg->has_mode) {
        dsd_apply_decode_mode_symbol_timing(cfg->decode_mode, old_runtime_input_rate, state);
        return;
    }
    restore_symbol_timing_snapshot(state, old_samples_per_symbol, old_symbol_center, old_jitter);
}

static void
apply_cfg_file_runtime_rate(dsd_opts* opts, dsd_state* state, const dsdneoUserConfig* cfg, int old_runtime_input_rate,
                            int old_samples_per_symbol, int old_symbol_center, int old_jitter) {
    if (!cfg_file_runtime_should_apply(opts, state, cfg)) {
        return;
    }

    const int configured_effective_input_rate = cfg_file_runtime_configured_rate(opts);
    if (opts->audio_in_type == AUDIO_IN_WAV) {
        cfg_file_runtime_apply_wav_input(opts, state, cfg, old_samples_per_symbol, old_symbol_center, old_jitter,
                                         configured_effective_input_rate);
        return;
    }

    cfg_file_runtime_apply_live_input(opts, state, cfg, old_runtime_input_rate, old_samples_per_symbol,
                                      old_symbol_center, old_jitter);
}

static void
apply_cfg_file_hot_restart(dsd_opts* opts, dsd_state* state, const dsdneoUserConfig* cfg, const char* old_audio_in_dev,
                           int old_audio_in_type, int old_wav_sample_rate, int old_effective_input_rate) {
    if (!cfg->has_input || cfg->input_source != DSDCFG_INPUT_FILE || old_audio_in_type != AUDIO_IN_WAV
        || strncmp(old_audio_in_dev, opts->audio_in_dev, sizeof opts->audio_in_dev) == 0) {
        return;
    }

    SNDFILE* new_audio_in_file = NULL;
    SF_INFO* new_audio_in_file_info = NULL;
    int configured_effective_input_rate = dsd_opts_effective_input_rate(opts);

    if (dsd_audio_open_mono_file_input(opts->audio_in_dev, opts->wav_sample_rate, &new_audio_in_file,
                                       &new_audio_in_file_info, NULL, NULL)
        != 0) {
        LOG_ERROR("Config: failed to open file input %s: %s\n", opts->audio_in_dev, sf_strerror(NULL));
        rollback_cfg_file_hot_restart(opts, state, old_audio_in_dev, old_audio_in_type, old_wav_sample_rate,
                                      old_effective_input_rate, configured_effective_input_rate);
        return;
    }

    if (opts->audio_in_file) {
        sf_close(opts->audio_in_file);
    }
    free(opts->audio_in_file_info);

    opts->audio_in_file = new_audio_in_file;
    opts->audio_in_file_info = new_audio_in_file_info;
    opts->audio_in_type = AUDIO_IN_WAV;
    dsd_opts_reset_pcm_input_state(opts);
}

static void
apply_cfg_pulse_in_hot_restart(dsd_opts* opts, const dsdneoUserConfig* cfg, const char* old_audio_in_dev,
                               int old_audio_in_type) {
    if (!cfg->has_input || cfg->input_source != DSDCFG_INPUT_PULSE || old_audio_in_type != AUDIO_IN_PULSE
        || opts->audio_in_type != AUDIO_IN_PULSE) {
        return;
    }
    if (strncmp(old_audio_in_dev, opts->audio_in_dev, sizeof opts->audio_in_dev) == 0
        && strncmp(old_audio_in_dev, "pulse", 5) == 0) {
        return;
    }

    closeAudioInput(opts);
    if (strncmp(opts->audio_in_dev, "pulse", 5) == 0 && opts->audio_in_dev[5] == ':' && opts->audio_in_dev[6] != '\0') {
        char tmp[128] = {0};
        DSD_SNPRINTF(tmp, sizeof tmp, "%s", opts->audio_in_dev + 6);
        parse_audio_input_string(opts, tmp);
    } else {
        opts->pa_input_idx[0] = '\0';
    }
    if (openAudioInput(opts) != 0) {
        LOG_ERROR("Config: failed to open PulseAudio input\n");
    }
}

static void
apply_cfg_pulse_out_hot_restart(dsd_opts* opts, const dsdneoUserConfig* cfg, const char* old_audio_out_dev,
                                int old_audio_out_type) {
    if (!cfg->has_output || cfg->output_backend != DSDCFG_OUTPUT_PULSE || old_audio_out_type != 0
        || opts->audio_out_type != 0) {
        return;
    }
    if (strncmp(old_audio_out_dev, opts->audio_out_dev, sizeof opts->audio_out_dev) == 0
        && strncmp(old_audio_out_dev, "pulse", 5) == 0) {
        return;
    }

    closeAudioOutput(opts);
    if (strncmp(opts->audio_out_dev, "pulse", 5) == 0 && opts->audio_out_dev[5] == ':'
        && opts->audio_out_dev[6] != '\0') {
        char tmp[128] = {0};
        DSD_SNPRINTF(tmp, sizeof tmp, "%s", opts->audio_out_dev + 6);
        parse_audio_output_string(opts, tmp);
    } else {
        opts->pa_output_idx[0] = '\0';
    }
    if (openAudioOutput(opts) != 0) {
        LOG_ERROR("Config: failed to open PulseAudio output\n");
    }
}

static void
apply_cfg_runtime_hot_switches(dsd_opts* opts, dsd_state* state, const dsdneoUserConfig* cfg,
                               const char* old_audio_in_dev, int old_audio_in_type, int old_wav_sample_rate,
                               int old_effective_input_rate, const char* old_audio_out_dev, int old_audio_out_type) {
    /* Tighten runtime behavior when applying configs mid-run by restarting
     * active backends whose configuration changed, while avoiding cross-backend
     * hot-switches. */
#ifdef USE_RADIO
    apply_cfg_rtl_hot_restart(opts, state, cfg, old_audio_in_dev, old_audio_in_type);
#else
    (void)state;
#endif
    apply_cfg_tcp_hot_restart(opts, cfg, old_audio_in_dev, old_audio_in_type);
    apply_cfg_udp_hot_restart(opts, cfg, old_audio_in_dev, old_audio_in_type);
    apply_cfg_file_hot_restart(opts, state, cfg, old_audio_in_dev, old_audio_in_type, old_wav_sample_rate,
                               old_effective_input_rate);
    apply_cfg_pulse_in_hot_restart(opts, cfg, old_audio_in_dev, old_audio_in_type);
    apply_cfg_pulse_out_hot_restart(opts, cfg, old_audio_out_dev, old_audio_out_type);
}

int
dsd_app_command_submit(int cmd_id, const void* payload, size_t payload_sz) {
    ensure_mu_init();
    dsd_mutex_lock(&g_mu);
    if (ui_cmd_is_coalescible_setter(cmd_id)) {
        struct dsd_app_command* pending = ui_cmd_find_pending_tail_unlocked(cmd_id);
        if (pending) {
            ui_cmd_store_payload(pending, cmd_id, payload, payload_sz);
            dsd_mutex_unlock(&g_mu);
            return DSD_APP_COMMAND_SUBMIT_COALESCED;
        }
    }
    if (q_is_full_unlocked()) {
        // Drop the oldest command (advance head) and warn once per burst
        g_head = (g_head + 1) % DSD_APP_CMD_Q_CAP;
        atomic_fetch_add(&g_overflow, 1);
        if (atomic_exchange(&g_overflow_warn_gate, 1) == 0) {
            LOG_WARN("WARNING: app_command_queue: overflow; dropping oldest command(s).\n");
        }
    }
    struct dsd_app_command* c = &g_q[g_tail];
    ui_cmd_store_payload(c, cmd_id, payload, payload_sz);
    g_tail = (g_tail + 1) % DSD_APP_CMD_Q_CAP;
    dsd_mutex_unlock(&g_mu);
    return DSD_APP_COMMAND_SUBMIT_QUEUED;
}

static const int k_ui_cmd_action_ids[] = {
    DSD_APP_CMD_TOGGLE_MUTE,
    DSD_APP_CMD_TOGGLE_COMPACT,
    DSD_APP_CMD_HISTORY_CYCLE,
    DSD_APP_CMD_SLOT1_TOGGLE,
    DSD_APP_CMD_SLOT2_TOGGLE,
    DSD_APP_CMD_SLOT_PREF_CYCLE,
    DSD_APP_CMD_TRUNK_TOGGLE,
    DSD_APP_CMD_SCANNER_TOGGLE,
    DSD_APP_CMD_PAYLOAD_TOGGLE,
    DSD_APP_CMD_P25_GA_TOGGLE,
    DSD_APP_CMD_LPF_TOGGLE,
    DSD_APP_CMD_HPF_TOGGLE,
    DSD_APP_CMD_PBF_TOGGLE,
    DSD_APP_CMD_HPF_D_TOGGLE,
    DSD_APP_CMD_AGGR_SYNC_TOGGLE,
    DSD_APP_CMD_CALL_ALERT_TOGGLE,
    DSD_APP_CMD_CONST_TOGGLE,
    DSD_APP_CMD_CONST_NORM_TOGGLE,
    DSD_APP_CMD_EYE_TOGGLE,
    DSD_APP_CMD_EYE_UNICODE_TOGGLE,
    DSD_APP_CMD_EYE_COLOR_TOGGLE,
    DSD_APP_CMD_FSK_HIST_TOGGLE,
    DSD_APP_CMD_SPECTRUM_TOGGLE,
    DSD_APP_CMD_INPUT_VOL_CYCLE,
    DSD_APP_CMD_EH_NEXT,
    DSD_APP_CMD_EH_PREV,
    DSD_APP_CMD_EH_TOGGLE_SLOT,
    DSD_APP_CMD_INVERT_TOGGLE,
    DSD_APP_CMD_MOD_TOGGLE,
    DSD_APP_CMD_DMR_RESET,
    DSD_APP_CMD_INPUT_MONITOR_TOGGLE,
    DSD_APP_CMD_COSINE_FILTER_TOGGLE,
    DSD_APP_CMD_TCP_CONNECT_AUDIO,
    DSD_APP_CMD_RIGCTL_CONNECT,
    DSD_APP_CMD_RETURN_CC,
    DSD_APP_CMD_CHANNEL_CYCLE,
    DSD_APP_CMD_SYMCAP_SAVE,
    DSD_APP_CMD_SYMCAP_STOP,
    DSD_APP_CMD_REPLAY_LAST,
    DSD_APP_CMD_WAV_START,
    DSD_APP_CMD_WAV_STOP,
    DSD_APP_CMD_WAV_TOGGLE,
    DSD_APP_CMD_STOP_PLAYBACK,
    DSD_APP_CMD_TRUNK_WLIST_TOGGLE,
    DSD_APP_CMD_TRUNK_PRIV_TOGGLE,
    DSD_APP_CMD_TRUNK_DATA_TOGGLE,
    DSD_APP_CMD_TRUNK_ENC_TOGGLE,
    DSD_APP_CMD_QUIT,
    DSD_APP_CMD_FORCE_PRIV_TOGGLE,
    DSD_APP_CMD_FORCE_RC4_TOGGLE,
    DSD_APP_CMD_TRUNK_GROUP_TOGGLE,
    DSD_APP_CMD_SIM_NOCAR,
    DSD_APP_CMD_MOD_P2_TOGGLE,
    DSD_APP_CMD_M17_TX_TOGGLE,
    DSD_APP_CMD_PROVOICE_ESK_TOGGLE,
    DSD_APP_CMD_PROVOICE_MODE_TOGGLE,
    DSD_APP_CMD_UI_MSG_CLEAR,
    DSD_APP_CMD_EH_RESET,
    DSD_APP_CMD_EVENT_LOG_DISABLE,
    DSD_APP_CMD_LCW_RETUNE_TOGGLE,
    DSD_APP_CMD_P25_CC_CAND_TOGGLE,
    DSD_APP_CMD_REVERSE_MUTE_TOGGLE,
    DSD_APP_CMD_DMR_LE_TOGGLE,
    DSD_APP_CMD_ALL_MUTES_TOGGLE,
    DSD_APP_CMD_INV_X2_TOGGLE,
    DSD_APP_CMD_INV_DMR_TOGGLE,
    DSD_APP_CMD_INV_DPMR_TOGGLE,
    DSD_APP_CMD_INV_M17_TOGGLE,
    DSD_APP_CMD_INPUT_SET_PULSE,
    DSD_APP_CMD_RTL_ENABLE_INPUT,
    DSD_APP_CMD_RTL_RESTART,
    DSD_APP_CMD_LRRP_SET_HOME,
    DSD_APP_CMD_LRRP_SET_DSDP,
    DSD_APP_CMD_LRRP_DISABLE,
    DSD_APP_CMD_UI_SHOW_DSP_PANEL_TOGGLE,
    DSD_APP_CMD_UI_SHOW_P25_METRICS_TOGGLE,
    DSD_APP_CMD_UI_SHOW_P25_AFFIL_TOGGLE,
    DSD_APP_CMD_UI_SHOW_P25_NEIGHBORS_TOGGLE,
    DSD_APP_CMD_UI_SHOW_P25_IDEN_TOGGLE,
    DSD_APP_CMD_UI_SHOW_P25_CCC_TOGGLE,
    DSD_APP_CMD_UI_SHOW_CHANNELS_TOGGLE,
    DSD_APP_CMD_UI_SHOW_P25_CALLSIGN_TOGGLE,
};

static const int k_ui_cmd_i32_ids[] = {
    DSD_APP_CMD_GAIN_DELTA,          DSD_APP_CMD_AGAIN_DELTA,      DSD_APP_CMD_SPEC_SIZE_DELTA,
    DSD_APP_CMD_PPM_DELTA,           DSD_APP_CMD_GAIN_SET,         DSD_APP_CMD_AGAIN_SET,
    DSD_APP_CMD_RTL_SET_DEV,         DSD_APP_CMD_RTL_SET_GAIN,     DSD_APP_CMD_RTL_SET_PPM,
    DSD_APP_CMD_RTL_SET_BW,          DSD_APP_CMD_RTL_SET_VOL_MULT, DSD_APP_CMD_RTL_SET_BIAS_TEE,
    DSD_APP_CMD_RTLTCP_SET_AUTOTUNE, DSD_APP_CMD_RTL_SET_AUTO_PPM, DSD_APP_CMD_RIGCTL_SET_MOD_BW,
    DSD_APP_CMD_SLOT_PREF_SET,       DSD_APP_CMD_SLOTS_ONOFF_SET,  DSD_APP_CMD_INPUT_VOL_SET,
};

static int
ui_cmd_id_in_list(int cmd_id, const int* ids, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (ids[i] == cmd_id) {
            return 1;
        }
    }
    return 0;
}

static int
ui_cmd_is_action_id(int cmd_id) {
    return ui_cmd_id_in_list(cmd_id, k_ui_cmd_action_ids, sizeof k_ui_cmd_action_ids / sizeof k_ui_cmd_action_ids[0]);
}

int
dsd_app_command_action(int cmd_id) {
    return ui_cmd_is_action_id(cmd_id) ? dsd_app_command_submit(cmd_id, NULL, 0U) : DSD_APP_COMMAND_SUBMIT_REJECTED;
}

int
dsd_app_command_set_i32(int cmd_id, int32_t value) {
    return ui_cmd_id_in_list(cmd_id, k_ui_cmd_i32_ids, sizeof k_ui_cmd_i32_ids / sizeof k_ui_cmd_i32_ids[0])
               ? dsd_app_command_submit(cmd_id, &value, sizeof value)
               : DSD_APP_COMMAND_SUBMIT_REJECTED;
}

int
dsd_app_command_set_u8(int cmd_id, uint8_t value) {
    switch (cmd_id) {
        case DSD_APP_CMD_TG_HOLD_TOGGLE:
        case DSD_APP_CMD_CALL_ALERT_EVENTS_SET:
        case DSD_APP_CMD_LOCKOUT_SLOT: return dsd_app_command_submit(cmd_id, &value, sizeof value);
        default: return DSD_APP_COMMAND_SUBMIT_REJECTED;
    }
}

int
dsd_app_command_set_u32(int cmd_id, uint32_t value) {
    switch (cmd_id) {
        case DSD_APP_CMD_RTL_SET_FREQ:
        case DSD_APP_CMD_TG_HOLD_SET:
        case DSD_APP_CMD_KEY_BASIC_SET:
        case DSD_APP_CMD_KEY_SCRAMBLER_SET: return dsd_app_command_submit(cmd_id, &value, sizeof value);
        default: return DSD_APP_COMMAND_SUBMIT_REJECTED;
    }
}

int
dsd_app_command_set_u64(int cmd_id, uint64_t value) {
    if (cmd_id != DSD_APP_CMD_KEY_RC4DES_SET) {
        return DSD_APP_COMMAND_SUBMIT_REJECTED;
    }
    return dsd_app_command_submit(cmd_id, &value, sizeof value);
}

int
dsd_app_command_set_double(int cmd_id, double value) {
    switch (cmd_id) {
        case DSD_APP_CMD_INPUT_WARN_DB_SET:
        case DSD_APP_CMD_RTL_SET_SQL_DB:
        case DSD_APP_CMD_HANGTIME_SET: return dsd_app_command_submit(cmd_id, &value, sizeof value);
        default: return DSD_APP_COMMAND_SUBMIT_REJECTED;
    }
}

int
dsd_app_command_set_float(int cmd_id, float value) {
    if (cmd_id != DSD_APP_CMD_CONST_GATE_DELTA) {
        return DSD_APP_COMMAND_SUBMIT_REJECTED;
    }
    return dsd_app_command_submit(cmd_id, &value, sizeof value);
}

int
dsd_app_command_set_string(int cmd_id, const char* value) {
    if (!value) {
        return DSD_APP_COMMAND_SUBMIT_REJECTED;
    }
    return ui_cmd_id_in_list(cmd_id, k_ui_cmd_string_ids, sizeof k_ui_cmd_string_ids / sizeof k_ui_cmd_string_ids[0])
               ? dsd_app_command_submit(cmd_id, value, strlen(value) + 1U)
               : DSD_APP_COMMAND_SUBMIT_REJECTED;
}

int
dsd_app_command_set_endpoint(int cmd_id, const char* host, int32_t port) {
    if (!host) {
        return DSD_APP_COMMAND_SUBMIT_REJECTED;
    }
    switch (cmd_id) {
        case DSD_APP_CMD_UDP_OUT_CFG:
        case DSD_APP_CMD_TCP_CONNECT_AUDIO_CFG:
        case DSD_APP_CMD_RIGCTL_CONNECT_CFG: {
            dsd_app_endpoint_payload payload = {0};
            DSD_SNPRINTF(payload.host, sizeof payload.host, "%s", host);
            payload.port = port;
            return dsd_app_command_submit(cmd_id, &payload, sizeof payload);
        }
        case DSD_APP_CMD_UDP_INPUT_CFG: {
            dsd_app_udp_input_payload payload = {0};
            DSD_SNPRINTF(payload.bind, sizeof payload.bind, "%s", host);
            payload.port = port;
            return dsd_app_command_submit(cmd_id, &payload, sizeof payload);
        }
        default: return DSD_APP_COMMAND_SUBMIT_REJECTED;
    }
}

int
dsd_app_command_set_p25_p2_params(const dsd_app_p25_p2_params_payload* payload) {
    return payload ? dsd_app_command_submit(DSD_APP_CMD_P25_P2_PARAMS_SET, payload, sizeof *payload)
                   : DSD_APP_COMMAND_SUBMIT_REJECTED;
}

int
dsd_app_command_set_hytera_key(const dsd_app_hytera_key_payload* payload) {
    return payload ? dsd_app_command_submit(DSD_APP_CMD_KEY_HYTERA_SET, payload, sizeof *payload)
                   : DSD_APP_COMMAND_SUBMIT_REJECTED;
}

int
dsd_app_command_set_aes_key(const dsd_app_aes_key_payload* payload) {
    return payload ? dsd_app_command_submit(DSD_APP_CMD_KEY_AES_SET, payload, sizeof *payload)
                   : DSD_APP_COMMAND_SUBMIT_REJECTED;
}

int
dsd_app_command_dsp_op(const dsd_app_dsp_payload* payload) {
    return payload ? dsd_app_command_submit(DSD_APP_CMD_DSP_OP, payload, sizeof *payload)
                   : DSD_APP_COMMAND_SUBMIT_REJECTED;
}

int
dsd_app_command_apply_config(const dsdneoUserConfig* config) {
    return config ? dsd_app_command_submit(DSD_APP_CMD_CONFIG_APPLY, config, sizeof *config)
                  : DSD_APP_COMMAND_SUBMIT_REJECTED;
}

int
dsd_app_command_set_config_metadata(const dsd_app_config_metadata_payload* payload) {
    return payload ? dsd_app_command_submit(DSD_APP_CMD_CONFIG_METADATA_SET, payload, sizeof *payload)
                   : DSD_APP_COMMAND_SUBMIT_REJECTED;
}

static int
ui_cmd_payload_has_min_size(const struct dsd_app_command* c, size_t want) {
    return c != NULL && c->n >= want;
}

struct ui_cmd_payload_min_size_rule {
    int id;
    size_t min_size;
};

static const struct ui_cmd_payload_min_size_rule k_ui_cmd_payload_min_size_rules[] = {
    {DSD_APP_CMD_TG_HOLD_TOGGLE, sizeof(uint8_t)},
    {DSD_APP_CMD_CALL_ALERT_EVENTS_SET, sizeof(uint8_t)},
    {DSD_APP_CMD_LOCKOUT_SLOT, sizeof(uint8_t)},
    {DSD_APP_CMD_RTL_SET_FREQ, sizeof(uint32_t)},
    {DSD_APP_CMD_TG_HOLD_SET, sizeof(uint32_t)},
    {DSD_APP_CMD_KEY_BASIC_SET, sizeof(uint32_t)},
    {DSD_APP_CMD_KEY_SCRAMBLER_SET, sizeof(uint32_t)},
    {DSD_APP_CMD_KEY_RC4DES_SET, sizeof(uint64_t)},
    {DSD_APP_CMD_INPUT_WARN_DB_SET, sizeof(double)},
    {DSD_APP_CMD_RTL_SET_SQL_DB, sizeof(double)},
    {DSD_APP_CMD_HANGTIME_SET, sizeof(double)},
    {DSD_APP_CMD_CONST_GATE_DELTA, sizeof(float)},
    {DSD_APP_CMD_UDP_OUT_CFG, sizeof(dsd_app_endpoint_payload)},
    {DSD_APP_CMD_TCP_CONNECT_AUDIO_CFG, sizeof(dsd_app_endpoint_payload)},
    {DSD_APP_CMD_RIGCTL_CONNECT_CFG, sizeof(dsd_app_endpoint_payload)},
    {DSD_APP_CMD_UDP_INPUT_CFG, sizeof(dsd_app_udp_input_payload)},
    {DSD_APP_CMD_P25_P2_PARAMS_SET, sizeof(dsd_app_p25_p2_params_payload)},
    {DSD_APP_CMD_KEY_HYTERA_SET, sizeof(dsd_app_hytera_key_payload)},
    {DSD_APP_CMD_KEY_AES_SET, sizeof(dsd_app_aes_key_payload)},
    {DSD_APP_CMD_DSP_OP, sizeof(dsd_app_dsp_payload)},
    {DSD_APP_CMD_CONFIG_APPLY, sizeof(dsdneoUserConfig)},
    {DSD_APP_CMD_CONFIG_METADATA_SET, sizeof(dsd_app_config_metadata_payload)},
};

static size_t
ui_cmd_payload_min_size_for_id(int cmd_id) {
    for (size_t i = 0; i < sizeof k_ui_cmd_payload_min_size_rules / sizeof k_ui_cmd_payload_min_size_rules[0]; i++) {
        if (k_ui_cmd_payload_min_size_rules[i].id == cmd_id) {
            return k_ui_cmd_payload_min_size_rules[i].min_size;
        }
    }
    return 0U;
}

static int
ui_cmd_payload_is_valid(const struct dsd_app_command* c) {
    if (!c) {
        return 0;
    }
    if (c->payload_truncated && !ui_cmd_is_string_payload_id(c->id)) {
        return 0;
    }
    if (ui_cmd_is_action_id(c->id)) {
        return c->n == 0U;
    }
    if (ui_cmd_id_in_list(c->id, k_ui_cmd_i32_ids, sizeof k_ui_cmd_i32_ids / sizeof k_ui_cmd_i32_ids[0])) {
        return ui_cmd_payload_has_min_size(c, sizeof(int32_t));
    }
    if (ui_cmd_id_in_list(c->id, k_ui_cmd_string_ids, sizeof k_ui_cmd_string_ids / sizeof k_ui_cmd_string_ids[0])) {
        return c->n > 0U;
    }
    const size_t min_size = ui_cmd_payload_min_size_for_id(c->id);
    return min_size == 0U || ui_cmd_payload_has_min_size(c, min_size);
}

static int
apply_cmd_basic_a(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    switch (c->id) {
        case DSD_APP_CMD_QUIT: exitflag = 1; return 1;
        case DSD_APP_CMD_FORCE_PRIV_TOGGLE:
            if (!state) {
                return 1;
            }
            if (state->M == 1 || state->M == 0x21) {
                state->M = 0;
            } else {
                state->M = 1;
            }
            return 1;
        case DSD_APP_CMD_FORCE_RC4_TOGGLE:
            if (!state) {
                return 1;
            }
            if (state->M == 1 || state->M == 0x21) {
                state->M = 0;
            } else {
                state->M = 0x21;
            }
            return 1;
        case DSD_APP_CMD_TOGGLE_COMPACT:
            opts->frontend_terminal_display.terminal_compact = opts->frontend_terminal_display.terminal_compact ? 0 : 1;
            return 1;
        case DSD_APP_CMD_HISTORY_CYCLE:
            (void)dsd_app_frontend_history_cycle_mode();
            dsd_telemetry_request_redraw();
            return 1;
        default: return 0;
    }
}

static int
apply_cmd_slot_controls(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    switch (c->id) {
        case DSD_APP_CMD_SLOT1_TOGGLE:
            if (!state) {
                return 1;
            }
            if (opts->slot1_on == 1) {
                opts->slot1_on = 0;
                if (opts->slot_preference == 0) {
                    opts->slot_preference = 2;
                }
                state->audio_out_float_buf_p = state->audio_out_float_buf + 100;
                state->audio_out_buf_p = state->audio_out_buf + 100;
                DSD_MEMSET(state->audio_out_float_buf, 0, 100 * sizeof(float));
                DSD_MEMSET(state->audio_out_buf, 0, 100 * sizeof(short));
                state->audio_out_idx2 = 0;
                state->audio_out_idx = 0;
            } else {
                opts->slot1_on = 1;
            }
            return 1;
        case DSD_APP_CMD_SLOT2_TOGGLE:
            if (!state) {
                return 1;
            }
            if (opts->slot2_on == 1) {
                opts->slot2_on = 0;
                opts->slot_preference = 0;
                state->audio_out_float_buf_pR = state->audio_out_float_bufR + 100;
                state->audio_out_buf_pR = state->audio_out_bufR + 100;
                DSD_MEMSET(state->audio_out_float_bufR, 0, 100 * sizeof(float));
                DSD_MEMSET(state->audio_out_bufR, 0, 100 * sizeof(short));
                state->audio_out_idx2R = 0;
                state->audio_out_idxR = 0;
            } else {
                opts->slot2_on = 1;
            }
            return 1;
        case DSD_APP_CMD_SLOT_PREF_CYCLE:
            if (opts->slot_preference == 0 || opts->slot_preference == 1) {
                opts->slot_preference++;
            } else {
                opts->slot_preference = 0;
            }
            return 1;
        default: return 0;
    }
}

static int
ui_cmd_handle_payload_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    (void)c;
    opts->payload = opts->payload ? 0 : 1;
    return 1;
}

static int
ui_cmd_handle_p25_ga_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    opts->frontend_display.show_p25_group_affiliations = opts->frontend_display.show_p25_group_affiliations ? 0 : 1;
    if (state) {
        DSD_SNPRINTF(state->ui_msg, sizeof state->ui_msg, "P25 Group Affiliation: %s",
                     opts->frontend_display.show_p25_group_affiliations ? "On" : "Off");
        state->ui_msg_expire = time(NULL) + 3;
    }
    return 1;
}

static int
ui_cmd_handle_lpf_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    (void)c;
    opts->use_lpf = opts->use_lpf ? 0 : 1;
    return 1;
}

static int
ui_cmd_handle_hpf_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    (void)c;
    opts->use_hpf = opts->use_hpf ? 0 : 1;
    return 1;
}

static int
ui_cmd_handle_pbf_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    (void)c;
    opts->use_pbf = opts->use_pbf ? 0 : 1;
    return 1;
}

static int
ui_cmd_handle_hpf_d_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    (void)c;
    opts->use_hpf_d = opts->use_hpf_d ? 0 : 1;
    return 1;
}

static int
ui_cmd_handle_aggr_sync_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    (void)c;
    opts->aggressive_framesync = opts->aggressive_framesync ? 0 : 1;
    return 1;
}

static int
ui_cmd_handle_call_alert_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    (void)c;
    uint8_t events = dsd_call_alert_mask_events(opts->call_alert_events);
    opts->call_alert_events = events;
    opts->call_alert = opts->call_alert ? 0 : (events ? 1 : 0);
    return 1;
}

static int
ui_cmd_handle_call_alert_events_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    uint8_t events = 0;
    if (c->n >= sizeof events) {
        DSD_MEMCPY(&events, c->data, sizeof events);
        events = dsd_call_alert_mask_events(events);
        opts->call_alert = events ? 1 : 0;
        opts->call_alert_events = events;
    }
    return 1;
}

static int
apply_cmd_payload_filters(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    static const struct dsd_app_command_handler_entry k_handlers[] = {
        {DSD_APP_CMD_PAYLOAD_TOGGLE, ui_cmd_handle_payload_toggle},
        {DSD_APP_CMD_P25_GA_TOGGLE, ui_cmd_handle_p25_ga_toggle},
        {DSD_APP_CMD_LPF_TOGGLE, ui_cmd_handle_lpf_toggle},
        {DSD_APP_CMD_HPF_TOGGLE, ui_cmd_handle_hpf_toggle},
        {DSD_APP_CMD_PBF_TOGGLE, ui_cmd_handle_pbf_toggle},
        {DSD_APP_CMD_HPF_D_TOGGLE, ui_cmd_handle_hpf_d_toggle},
        {DSD_APP_CMD_AGGR_SYNC_TOGGLE, ui_cmd_handle_aggr_sync_toggle},
        {DSD_APP_CMD_CALL_ALERT_TOGGLE, ui_cmd_handle_call_alert_toggle},
        {DSD_APP_CMD_CALL_ALERT_EVENTS_SET, ui_cmd_handle_call_alert_events_set},
    };
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

static int
apply_cmd_constellation(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    switch (c->id) {
        case DSD_APP_CMD_CONST_TOGGLE:
            if (opts->audio_in_type == AUDIO_IN_RTL) {
                opts->frontend_display.constellation = opts->frontend_display.constellation ? 0 : 1;
            }
            return 1;
        case DSD_APP_CMD_CONST_NORM_TOGGLE:
            if (opts->audio_in_type == AUDIO_IN_RTL && opts->frontend_display.constellation == 1) {
                opts->frontend_display.const_norm_mode = (opts->frontend_display.const_norm_mode == 0) ? 1 : 0;
            }
            return 1;
        case DSD_APP_CMD_CONST_GATE_DELTA:
            if (opts->audio_in_type == AUDIO_IN_RTL && opts->frontend_display.constellation == 1) {
                float d = 0.0f;
                if (c->n >= (int)sizeof(float)) {
                    DSD_MEMCPY(&d, c->data, sizeof(float));
                }
                float* g = (opts->mod_qpsk == 1) ? &opts->frontend_display.const_gate_qpsk
                                                 : &opts->frontend_display.const_gate_other;
                *g += d;
                if (*g < 0.0f) {
                    *g = 0.0f;
                }
                if (*g > 0.90f) {
                    *g = 0.90f;
                }
            }
            return 1;
        default: return 0;
    }
}

static int
ui_cmd_handle_eye_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    (void)c;
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        opts->frontend_display.eye_view = opts->frontend_display.eye_view ? 0 : 1;
    }
    return 1;
}

static int
ui_cmd_handle_eye_unicode_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    (void)c;
    if (opts->audio_in_type == AUDIO_IN_RTL && opts->frontend_display.eye_view == 1) {
        opts->frontend_terminal_display.eye_unicode = opts->frontend_terminal_display.eye_unicode ? 0 : 1;
    }
    return 1;
}

static int
ui_cmd_handle_eye_color_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    (void)c;
    if (opts->audio_in_type == AUDIO_IN_RTL && opts->frontend_display.eye_view == 1) {
        opts->frontend_terminal_display.eye_color = opts->frontend_terminal_display.eye_color ? 0 : 1;
    }
    return 1;
}

static int
ui_cmd_handle_fsk_hist_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    (void)c;
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        opts->frontend_display.fsk_hist_view = opts->frontend_display.fsk_hist_view ? 0 : 1;
    }
    return 1;
}

static int
ui_cmd_handle_spectrum_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    (void)c;
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        opts->frontend_display.spectrum_view = opts->frontend_display.spectrum_view ? 0 : 1;
    }
    return 1;
}

static int
ui_cmd_handle_spec_size_delta(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int32_t d = 0;
    (void)state;
    if (c->n >= (int)sizeof(int32_t)) {
        DSD_MEMCPY(&d, c->data, sizeof(int32_t));
    }
    if (opts->audio_in_type == AUDIO_IN_RTL && opts->frontend_display.spectrum_view == 1) {
        /* Keep toggle state normalized while applying size changes. */
        opts->frontend_display.spectrum_view = 1;
#ifdef USE_RADIO
        int n = rtl_stream_spectrum_get_size();
        int want = n + d;
        if (want < 64) {
            want = 64;
        }
        if (want > 1024) {
            want = 1024;
        }
        if (want != n) {
            (void)rtl_stream_spectrum_set_size(want);
        }
#else
        (void)d;
#endif
    }
    return 1;
}

static int
apply_cmd_eye_spectrum(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    static const struct dsd_app_command_handler_entry k_handlers[] = {
        {DSD_APP_CMD_EYE_TOGGLE, ui_cmd_handle_eye_toggle},
        {DSD_APP_CMD_EYE_UNICODE_TOGGLE, ui_cmd_handle_eye_unicode_toggle},
        {DSD_APP_CMD_EYE_COLOR_TOGGLE, ui_cmd_handle_eye_color_toggle},
        {DSD_APP_CMD_FSK_HIST_TOGGLE, ui_cmd_handle_fsk_hist_toggle},
        {DSD_APP_CMD_SPECTRUM_TOGGLE, ui_cmd_handle_spectrum_toggle},
        {DSD_APP_CMD_SPEC_SIZE_DELTA, ui_cmd_handle_spec_size_delta},
    };
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

static int
apply_cmd_trunk_controls(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    switch (c->id) {
        case DSD_APP_CMD_DMR_RESET:
            if (!state) {
                return 1;
            }
            state->dmr_rest_channel = -1;
            state->dmr_mfid = -1;
            DSD_SNPRINTF(state->dmr_branding_sub, sizeof state->dmr_branding_sub, "%s", "");
            DSD_SNPRINTF(state->dmr_branding, sizeof state->dmr_branding, "%s", "");
            DSD_SNPRINTF(state->dmr_site_parms, sizeof state->dmr_site_parms, "%s", "");
            opts->dmr_dmrla_is_set = 0;
            opts->dmr_dmrla_n = 0;
            state->nxdn_location_site_code = 0;
            state->nxdn_location_sys_code = 0;
            DSD_SNPRINTF(state->nxdn_location_category, sizeof state->nxdn_location_category, "%s", " ");
            state->nxdn_last_ran = -1;
            state->nxdn_ran = 0;
            state->nxdn_rcn = 0;
            state->nxdn_base_freq = 0;
            state->nxdn_step = 0;
            state->nxdn_bw = 0;
            return 1;
        case DSD_APP_CMD_TCP_CONNECT_AUDIO: {
            int rc = svc_tcp_connect_audio(opts, opts->tcp_hostname, opts->tcp_portno);
            if (rc == 0) {
                LOG_INFO("TCP Socket Connected Successfully.\n");
                ui_set_tcp_audio_connected_toast_if_output_ready(opts, state, opts->tcp_hostname, opts->tcp_portno);
            } else {
                LOG_ERROR("TCP Socket Connection Error.\n");
                ui_set_toast(state, 4, "TCP audio connect failed: %s:%d", opts->tcp_hostname, opts->tcp_portno);
            }
            return ui_cmd_apply_status_from_service_rc(rc);
        }
        case DSD_APP_CMD_RIGCTL_CONNECT:
            DSD_MEMCPY(opts->rigctlhostname, opts->tcp_hostname, sizeof(opts->rigctlhostname));
            opts->rigctl_sockfd = Connect(opts->rigctlhostname, opts->rigctlportno);
            opts->use_rigctl = (opts->rigctl_sockfd != DSD_INVALID_SOCKET) ? 1 : 0;
            if (opts->use_rigctl) {
                ui_set_toast(state, 3, "Rigctl connected: %s:%d", opts->rigctlhostname, opts->rigctlportno);
            } else {
                ui_set_toast(state, 4, "Rigctl connect failed: %s:%d", opts->rigctlhostname, opts->rigctlportno);
            }
            return 1;
        case DSD_APP_CMD_RETURN_CC: return apply_manual_return_to_cc(opts, state);
        case DSD_APP_CMD_SIM_NOCAR:
            if (!state) {
                return 1;
            }
            state->last_cc_sync_time = 0;
            state->last_vc_sync_time = 0;
            state->last_vc_sync_time_m = 0.0;
            noCarrier(opts, state);
            return 1;
        default: return 0;
    }
}

static int
apply_cmd_lockout_slot(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    if (!state) {
        return (c && c->id == DSD_APP_CMD_LOCKOUT_SLOT) ? 1 : 0;
    }
    if (c->id != DSD_APP_CMD_LOCKOUT_SLOT) {
        return 0;
    }
    uint8_t slot = 0;
    dsd_tg_policy_entry lockout_entry;
    char metadata[16];
    int upsert_rc = 0;
    if (c->n >= 1) {
        DSD_MEMCPY(&slot, c->data, 1);
    }
    if (opts->frame_provoice == 1) {
        return 1;
    }
    int tg = (slot == 0) ? state->lasttg : state->lasttgR;
    if (tg == 0) {
        return 1;
    }
    if (dsd_tg_policy_make_exact_entry((uint32_t)tg, "B", "LOCKOUT", DSD_TG_POLICY_SOURCE_USER_LOCKOUT, &lockout_entry)
        != 0) {
        return 1;
    }
    upsert_rc = dsd_tg_policy_upsert_exact(state, &lockout_entry, DSD_TG_POLICY_UPSERT_REPLACE_FIRST);
    if (upsert_rc != 0) {
        LOG_WARN("WARNING: User lockout for TG %d could not be applied (rc=%d); skipping persistence.\n", tg,
                 upsert_rc);
        return 1;
    }

    int eh_slot = (slot == 0) ? 0 : 1;
    DSD_SNPRINTF(state->event_history_s[eh_slot].Event_History_Items[0].internal_str,
                 sizeof state->event_history_s[eh_slot].Event_History_Items[0].internal_str,
                 "Target: %d; has been locked out; User Lock Out.", tg);
    dsd_event_history_mark_dirty(&state->event_history_s[eh_slot]);
    watchdog_event_current(opts, state, eh_slot);
    DSD_SNPRINTF(state->call_string[eh_slot], sizeof state->call_string[eh_slot], "%s",
                 "                     "); // 21 spaces

    DSD_SNPRINTF(metadata, sizeof(metadata), "%02X",
                 (unsigned int)((slot == 0) ? state->payload_algid : state->payload_algidR));
    if (dsd_tg_policy_append_group_file_row(opts, &lockout_entry, metadata) != 0) {
        LOG_WARN("WARNING: User lockout for TG %d was applied in-memory but could not be persisted to '%s'.\n", tg,
                 opts->group_in_file);
    }

    const int transition_status = apply_lockout_decoder_transition(opts, state);
    if (transition_status == UI_CMD_APPLY_FAILED) {
        LOG_WARN("WARNING: User lockout for TG %d was applied, but the return-to-CC cleanup tune was not accepted.\n",
                 tg);
        ui_set_toast(state, 4, "TG %d locked out; return-to-CC tune failed", tg);
    }
    return UI_CMD_APPLY_COMPLETED;
}

static int
apply_cmd_provoice_m17(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    switch (c->id) {
        case DSD_APP_CMD_M17_TX_TOGGLE:
            if (!state) {
                return 1;
            }
            if (opts->m17encoder == 1) {
                state->m17encoder_tx = (state->m17encoder_tx == 0) ? 1 : 0;
                if (state->m17encoder_tx == 0) {
                    state->m17encoder_eot = 1;
                }
            }
            return 1;
        case DSD_APP_CMD_PROVOICE_ESK_TOGGLE:
            if (!state) {
                return 1;
            }
            if (opts->frame_provoice == 1) {
                state->esk_mask = (state->esk_mask == 0) ? 0xA0 : 0;
            }
            return 1;
        case DSD_APP_CMD_PROVOICE_MODE_TOGGLE:
            if (!state) {
                return 1;
            }
            if (opts->frame_provoice == 1) {
                state->ea_mode = (state->ea_mode == 0) ? 1 : 0;
                state->edacs_site_id = 0;
                state->edacs_lcn_count = 0;
                state->edacs_cc_lcn = 0;
                state->edacs_vc_lcn = 0;
                state->edacs_tuned_lcn = -1;
                state->edacs_vc_call_type = 0;
                state->p25_cc_freq = 0;
                state->trunk_cc_freq = 0;
                opts->trunk_is_tuned = 0;
                state->lasttg = 0;
                state->lastsrc = 0;
            }
            return 1;
        default: return 0;
    }
}

static int
apply_cmd_channel_cycle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    if (c->id != DSD_APP_CMD_CHANNEL_CYCLE) {
        return 0;
    }
    if (!state) {
        return 1;
    }
    if (opts->use_rigctl == 1 || opts->audio_in_type == AUDIO_IN_RTL) {
        return apply_manual_channel_cycle(opts, state);
    }
    return 1;
}

static int
ui_cmd_handle_symcap_save(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    char timestr[7];
    char datestr[9];
    (void)dsd_format_local_datetime(time(NULL), DSD_LOCAL_DATETIME_TIME_COMPACT, timestr, sizeof timestr);
    (void)dsd_format_local_datetime(time(NULL), DSD_LOCAL_DATETIME_DATE_COMPACT, datestr, sizeof datestr);
    DSD_SNPRINTF(opts->symbol_out_file, sizeof opts->symbol_out_file, "%s_%s_dibit_capture.bin", datestr, timestr);
    openSymbolOutFile(opts, state);
    if (state && state->event_history_s) {
        char event_str[2000] = {0};
        DSD_SNPRINTF(event_str, sizeof event_str, "DSD-neo Dibit Capture File Started: %s;", opts->symbol_out_file);
        watchdog_event_datacall(opts, state, 0xFFFFFF, 0xFFFFFF, event_str, 0);
        dsd_event_history_item_set_metadata(&state->event_history_s[0].Event_History_Items[0], DSD_EVENT_SEVERITY_INFO,
                                            DSD_EVENT_CATEGORY_SYSTEM);
        dsd_event_history_mark_dirty(&state->event_history_s[0]);
        state->lastsrc = 0;
        dsd_event_sync_slot(opts, state, 0);
    }
    opts->symbol_out_file_creation_time = time(NULL);
    opts->symbol_out_file_is_auto = 1;
    return 1;
}

static int
ui_cmd_handle_symcap_stop(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    if (opts->symbol_out_f) {
        closeSymbolOutFile(opts, state);
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", opts->symbol_out_file);
        if (state && state->event_history_s) {
            char event_str[2000] = {0};
            DSD_SNPRINTF(event_str, sizeof event_str, "DSD-neo Dibit Capture File  Closed: %s;", opts->symbol_out_file);
            watchdog_event_datacall(opts, state, 0xFFFFFF, 0xFFFFFF, event_str, 0);
            dsd_event_history_item_set_metadata(&state->event_history_s[0].Event_History_Items[0],
                                                DSD_EVENT_SEVERITY_INFO, DSD_EVENT_CATEGORY_SYSTEM);
            dsd_event_history_mark_dirty(&state->event_history_s[0]);
            state->lastsrc = 0;
            dsd_event_sync_slot(opts, state, 0);
        }
    }
    opts->symbol_out_file_is_auto = 0;
    return 1;
}

static int
ui_cmd_handle_replay_last(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    dsd_stat_t sb;
    (void)c;
    if (dsd_stat_path(opts->audio_in_dev, &sb) != 0) {
        LOG_ERROR("Error, couldn't open %s\n", opts->audio_in_dev);
        return UI_CMD_APPLY_FAILED;
    }
    if (dsd_stat_is_regular(&sb)) {
        opts->symbolfile = dsd_fopen_existing_regular_file(opts->audio_in_dev, "rb");
        if (opts->symbolfile) {
            opts->audio_in_type = AUDIO_IN_SYMBOL_BIN;
            state->symbol_replay_format = DSD_SYMBOL_REPLAY_FORMAT_UNKNOWN;
            state->symbol_replay_header_checked = 0;
            state->symbol_replay_has_soft = 0;
            (void)ui_reconfigure_output_for_input_policy(opts, state);
        }
    }
    return 1;
}

static int
ui_cmd_handle_wav_start(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    if (opts->dmr_stereo_wav == 1 && (opts->wav_out_f != NULL || opts->wav_out_fR != NULL)) {
        return UI_CMD_APPLY_COMPLETED;
    }

    int rc = svc_enable_per_call_wav(opts, state);
    return ui_cmd_apply_status_from_service_rc(rc);
}

static int
ui_cmd_handle_wav_stop(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    opts->wav_out_f = close_and_rename_wav_file(opts->wav_out_f, opts, opts->wav_out_file, opts->wav_out_dir,
                                                state ? &state->event_history_s[0] : NULL);
    opts->wav_out_fR = close_and_rename_wav_file(opts->wav_out_fR, opts, opts->wav_out_fileR, opts->wav_out_dir,
                                                 state ? &state->event_history_s[1] : NULL);
    opts->wav_out_file[0] = 0;
    opts->wav_out_fileR[0] = 0;
    opts->dmr_stereo_wav = 0;
    return 1;
}

static int
ui_cmd_handle_wav_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    if (opts->dmr_stereo_wav == 1 && (opts->wav_out_f != NULL || opts->wav_out_fR != NULL)) {
        return ui_cmd_handle_wav_stop(opts, state, c);
    }
    return ui_cmd_handle_wav_start(opts, state, c);
}

static int
ui_cmd_handle_stop_playback(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    if (opts->symbolfile != NULL) {
        if (opts->audio_in_type == AUDIO_IN_SYMBOL_BIN) {
            fclose(opts->symbolfile);
        }
        opts->symbolfile = NULL;
    }
    if (opts->audio_in_type == AUDIO_IN_WAV && opts->audio_in_file) {
        sf_close(opts->audio_in_file);
        opts->audio_in_file = NULL;
    }
    if (opts->audio_out_type == 0) {
        opts->audio_in_type = AUDIO_IN_PULSE;
        if (openAudioInput(opts) != 0) {
            LOG_ERROR("UI: failed to open PulseAudio input\n");
        } else {
            (void)ui_reconfigure_output_for_input_policy(opts, state);
        }
    } else {
        opts->audio_in_type = AUDIO_IN_STDIN;
        (void)ui_reconfigure_output_for_input_policy(opts, state);
    }
    return 1;
}

static int
apply_cmd_capture_playback(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    static const struct dsd_app_command_handler_entry k_handlers[] = {
        {DSD_APP_CMD_SYMCAP_SAVE, ui_cmd_handle_symcap_save},     {DSD_APP_CMD_SYMCAP_STOP, ui_cmd_handle_symcap_stop},
        {DSD_APP_CMD_REPLAY_LAST, ui_cmd_handle_replay_last},     {DSD_APP_CMD_WAV_START, ui_cmd_handle_wav_start},
        {DSD_APP_CMD_WAV_STOP, ui_cmd_handle_wav_stop},           {DSD_APP_CMD_WAV_TOGGLE, ui_cmd_handle_wav_toggle},
        {DSD_APP_CMD_STOP_PLAYBACK, ui_cmd_handle_stop_playback},
    };
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

static int
ui_cmd_handle_lcw_retune_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    (void)c;
    svc_toggle_lcw_retune(opts);
    return 1;
}

static int
ui_cmd_handle_p25_cc_cand_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    (void)c;
    opts->p25_prefer_candidates = opts->p25_prefer_candidates ? 0 : 1;
    return 1;
}

static int
ui_cmd_handle_reverse_mute_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    (void)c;
    svc_toggle_reverse_mute(opts);
    return 1;
}

static int
ui_cmd_handle_config_metadata_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)opts;
    if (!state || !c || c->n < sizeof(dsd_app_config_metadata_payload)) {
        return UI_CMD_APPLY_INVALID_PAYLOAD;
    }
    dsd_app_config_metadata_payload payload;
    DSD_MEMCPY(&payload, c->data, sizeof payload);
    state->config_autosave_enabled = payload.autosave_enabled ? 1 : 0;
    DSD_SNPRINTF(state->config_autosave_path, sizeof state->config_autosave_path, "%s", payload.path);
    state->config_autosave_path[sizeof state->config_autosave_path - 1] = '\0';
    return UI_CMD_APPLY_COMPLETED;
}

static int
ui_cmd_handle_config_apply(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    if (!state || c->n < sizeof(dsdneoUserConfig)) {
        return UI_CMD_APPLY_INVALID_PAYLOAD;
    }

    dsdneoUserConfig cfg;
    char old_audio_in_dev[sizeof opts->audio_in_dev];
    char old_audio_out_dev[sizeof opts->audio_out_dev];
    int old_audio_in_type = opts->audio_in_type;
    int old_audio_out_type = opts->audio_out_type;
    int old_wav_sample_rate = opts->wav_sample_rate;
    int old_effective_input_rate = dsd_opts_effective_input_rate(opts);
    int old_runtime_input_rate = current_demod_rate(opts, state);
    int old_samples_per_symbol = state->samplesPerSymbol;
    int old_symbol_center = state->symbolCenter;
    int old_jitter = state->jitter;
    dsd_frontend_kind old_frontend_kind = opts->frontend_kind;

    DSD_SNPRINTF(old_audio_in_dev, sizeof old_audio_in_dev, "%s", opts->audio_in_dev);
    DSD_SNPRINTF(old_audio_out_dev, sizeof old_audio_out_dev, "%s", opts->audio_out_dev);

    DSD_MEMCPY(&cfg, c->data, sizeof cfg);
    dsd_apply_user_config_to_opts(&cfg, opts, state);
    /*
     * Frontend lifecycle is owned by startup and ui_start/ui_stop.
     * Runtime config/profile loads may carry persisted frontend defaults,
     * but flipping this live can strand an active frontend session before
     * the UI thread has a chance to shut it down.
     */
    opts->frontend_kind = old_frontend_kind;
#ifdef USE_RADIO
    apply_cfg_live_rtl_ppm_request(opts, &cfg, old_audio_in_type);
#endif
    apply_cfg_runtime_hot_switches(opts, state, &cfg, old_audio_in_dev, old_audio_in_type, old_wav_sample_rate,
                                   old_effective_input_rate, old_audio_out_dev, old_audio_out_type);
    restore_live_pcm_rate_after_staged_file_apply(opts, &cfg, old_wav_sample_rate);
    apply_cfg_file_runtime_rate(opts, state, &cfg, old_runtime_input_rate, old_samples_per_symbol, old_symbol_center,
                                old_jitter);
    int reconfigure_rc = ui_reconfigure_output_for_input_policy(opts, state);
    if (cfg.frontend_kind_is_set && cfg.frontend_kind != old_frontend_kind) {
        return UI_CMD_APPLY_RESTART_REQUIRED;
    }
    return (reconfigure_rc == 0) ? UI_CMD_APPLY_COMPLETED : UI_CMD_APPLY_FAILED;
}

static int
apply_cmd_misc_config(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    static const struct dsd_app_command_handler_entry k_handlers[] = {
        {DSD_APP_CMD_LCW_RETUNE_TOGGLE, ui_cmd_handle_lcw_retune_toggle},
        {DSD_APP_CMD_P25_CC_CAND_TOGGLE, ui_cmd_handle_p25_cc_cand_toggle},
        {DSD_APP_CMD_REVERSE_MUTE_TOGGLE, ui_cmd_handle_reverse_mute_toggle},
        {DSD_APP_CMD_CONFIG_APPLY, ui_cmd_handle_config_apply},
        {DSD_APP_CMD_CONFIG_METADATA_SET, ui_cmd_handle_config_metadata_set},
    };
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

static int
apply_cmd(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    static const dsd_app_command_handler_fn k_command_groups[] = {
        apply_cmd_basic_a,       apply_cmd_slot_controls,    apply_cmd_payload_filters, apply_cmd_constellation,
        apply_cmd_eye_spectrum,  apply_cmd_trunk_controls,   apply_cmd_lockout_slot,    apply_cmd_provoice_m17,
        apply_cmd_channel_cycle, apply_cmd_capture_playback, apply_cmd_misc_config,
    };
    if (!c) {
        return UI_CMD_APPLY_INVALID_PAYLOAD;
    }
    if (!opts) {
        if (c->id == DSD_APP_CMD_QUIT) {
            exitflag = 1;
            return UI_CMD_APPLY_COMPLETED;
        }
        return UI_CMD_APPLY_UNSUPPORTED;
    }
    int r = ui_cmd_dispatch(opts, state, c);
    if (r) {
        return r;
    }
    r = apply_cmd_ui_visibility(opts, c);
    if (r) {
        return r;
    }
    r = apply_cmd_key_management(opts, state, c);
    if (r) {
        return r;
    }
    r = apply_cmd_runtime_toggles(opts, c);
    if (r) {
        return r;
    }
    r = apply_cmd_io_and_import(opts, state, c);
    if (r) {
        return r;
    }
#ifdef USE_RADIO
    r = apply_cmd_dsp(c);
    if (r) {
        return r;
    }
#endif
    for (size_t i = 0; i < (sizeof k_command_groups / sizeof k_command_groups[0]); ++i) {
        r = k_command_groups[i](opts, state, c);
        if (r) {
            return r;
        }
    }
    return UI_CMD_APPLY_UNSUPPORTED;
}

int
dsd_app_drain_cmds(dsd_opts* opts, dsd_state* state) {
    int n_applied = 0;
    ensure_mu_init();
    for (;;) {
        struct dsd_app_command cmd;
        int have = 0;
        dsd_mutex_lock(&g_mu);
        if (!q_is_empty_unlocked()) {
            cmd = g_q[g_head];
            g_head = (g_head + 1) % DSD_APP_CMD_Q_CAP;
            have = 1;
        }
        // Reset overflow warning gate when queue has space again
        if (((g_tail + 1) % DSD_APP_CMD_Q_CAP) != g_head) {
            atomic_store(&g_overflow_warn_gate, 0);
        }
        dsd_mutex_unlock(&g_mu);
        if (!have) {
            break;
        }
        if (!ui_cmd_payload_is_valid(&cmd)) {
            n_applied++;
            continue;
        }
        (void)apply_cmd(opts, state, &cmd);
        // After applying a command, publish updated snapshots so the UI can
        // render consistent opts/state without racing live structures.
        dsd_telemetry_publish_opts_snapshot(opts);
        if (state) {
            dsd_telemetry_publish_snapshot(state);
        }
        n_applied++;
    }
    return n_applied;
}
