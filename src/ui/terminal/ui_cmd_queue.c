// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* UI → Demod command queue (SPSC, bounded) */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/crypto/dmr_keystream.h>
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
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/freq_parse.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/telemetry.h>
#include <dsd-neo/ui/menu_services.h>
#include <dsd-neo/ui/ui_async.h>
#include <dsd-neo/ui/ui_cmd.h>
#include <dsd-neo/ui/ui_cmd_dispatch.h>
#include <dsd-neo/ui/ui_dsp_cmd.h>
#include <dsd-neo/ui/ui_history.h>
#include <sndfile.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/dibit.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/platform.h"
#include "dsd-neo/runtime/call_alert.h"

#ifdef USE_RADIO
#endif

#define UI_CMD_Q_CAP 128

_Static_assert(sizeof(dsdneoUserConfig) <= sizeof(((struct UiCmd*)0)->data),
               "UiCmd payload too small for dsdneoUserConfig");

static struct UiCmd g_q[UI_CMD_Q_CAP];
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
ui_cmd_dispatch(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    const struct UiCmdReg* regs[] = {ui_actions_audio, ui_actions_radio, ui_actions_trunk, ui_actions_logging, NULL};
    for (int i = 0; regs[i] != NULL; ++i) {
        for (const struct UiCmdReg* r = regs[i]; r && r->fn != NULL; ++r) {
            if (r->id == c->id) {
                return r->fn(opts, state, c);
            }
        }
    }
    return 0; // not handled
}

static inline int
q_is_full_unlocked(void) {
    return ((g_tail + 1) % UI_CMD_Q_CAP) == g_head;
}

static inline int
q_is_empty_unlocked(void) {
    return g_head == g_tail;
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

#ifdef USE_RADIO
static inline int
ui_rc_is_not_supported(int rc) {
    return rc == DSD_ERR_NOT_SUPPORTED;
}
#endif

struct UiVisibilityToggleSpec {
    int cmd_id;
    size_t opts_offset;
};

static const struct UiVisibilityToggleSpec k_ui_visibility_toggle_specs[] = {
    {UI_CMD_UI_SHOW_DSP_PANEL_TOGGLE, offsetof(dsd_opts, show_dsp_panel)},
    {UI_CMD_UI_SHOW_P25_METRICS_TOGGLE, offsetof(dsd_opts, show_p25_metrics)},
    {UI_CMD_UI_SHOW_P25_AFFIL_TOGGLE, offsetof(dsd_opts, show_p25_affiliations)},
    {UI_CMD_UI_SHOW_P25_NEIGHBORS_TOGGLE, offsetof(dsd_opts, show_p25_neighbors)},
    {UI_CMD_UI_SHOW_P25_IDEN_TOGGLE, offsetof(dsd_opts, show_p25_iden_plan)},
    {UI_CMD_UI_SHOW_P25_CCC_TOGGLE, offsetof(dsd_opts, show_p25_cc_candidates)},
    {UI_CMD_UI_SHOW_CHANNELS_TOGGLE, offsetof(dsd_opts, show_channels)},
    {UI_CMD_UI_SHOW_P25_CALLSIGN_TOGGLE, offsetof(dsd_opts, show_p25_callsign_decode)},
};

static int
ui_apply_visibility_toggle(dsd_opts* opts, int cmd_id) {
    for (size_t i = 0; i < (sizeof k_ui_visibility_toggle_specs / sizeof k_ui_visibility_toggle_specs[0]); ++i) {
        if (k_ui_visibility_toggle_specs[i].cmd_id == cmd_id) {
            int* field = (int*)((char*)opts + k_ui_visibility_toggle_specs[i].opts_offset);
            *field = *field ? 0 : 1;
            return 1;
        }
    }
    return 0;
}

static int
apply_cmd_ui_visibility(dsd_opts* opts, const struct UiCmd* c) {
    if (!opts || !c) {
        return 0;
    }
    return ui_apply_visibility_toggle(opts, c->id);
}

static int
ui_cmd_copy_payload_string(const struct UiCmd* c, char* out, size_t out_sz) {
    if (!c || !out || out_sz == 0 || c->n == 0) {
        return 0;
    }
    size_t n = (c->n < out_sz - 1) ? c->n : out_sz - 1;
    DSD_MEMCPY(out, c->data, n);
    out[n] = '\0';
    return 1;
}

typedef int (*UiCmdHandlerFn)(dsd_opts* opts, dsd_state* state, const struct UiCmd* c);

struct UiCmdHandlerEntry {
    int cmd_id;
    UiCmdHandlerFn fn;
};

static int
ui_cmd_apply_handler_table(const struct UiCmdHandlerEntry* entries, size_t count, dsd_opts* opts, dsd_state* state,
                           const struct UiCmd* c) {
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
apply_cmd_key_management_basic(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    if (!opts || !state || !c) {
        return 0;
    }
    switch (c->id) {
        case UI_CMD_KEY_BASIC_SET: {
            if (c->n >= (int)sizeof(uint32_t)) {
                uint32_t v = 0;
                DSD_MEMCPY(&v, c->data, sizeof v);
                state->K = v;
                ui_cmd_reset_key_mute_state(opts, state);
            }
            return 1;
        }
        case UI_CMD_KEY_SCRAMBLER_SET: {
            if (c->n >= (int)sizeof(uint32_t)) {
                uint32_t v = 0;
                DSD_MEMCPY(&v, c->data, sizeof v);
                state->R = v;
                ui_cmd_reset_key_mute_state(opts, state);
            }
            return 1;
        }
        case UI_CMD_KEY_RC4DES_SET: {
            if (c->n >= (int)sizeof(uint64_t)) {
                uint64_t v = 0;
                DSD_MEMCPY(&v, c->data, sizeof v);
                state->R = v;
                state->RR = v;
                ui_cmd_reset_key_mute_state(opts, state);
            }
            return 1;
        }
        default: return 0;
    }
}

static int
apply_cmd_key_management_block_keys(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    if (!opts || !state || !c) {
        return 0;
    }
    switch (c->id) {
        case UI_CMD_KEY_HYTERA_SET: {
            if (c->n >= (int)(sizeof(uint64_t) * 5)) {
                struct {
                    uint64_t H, K1, K2, K3, K4;
                } p;

                DSD_MEMCPY(&p, c->data, sizeof p);
                state->H = p.H;
                state->K1 = p.K1;
                state->K2 = p.K2;
                state->K3 = p.K3;
                state->K4 = p.K4;
                ui_cmd_reset_key_mute_state(opts, state);
                DSD_SNPRINTF(state->ui_msg, sizeof state->ui_msg, "Hytera key loaded (%s)",
                             (state->M == 1) ? "forced" : "not forced");
                state->ui_msg_expire = time(NULL) + 5;
            }
            return 1;
        }
        case UI_CMD_KEY_AES_SET: {
            if (c->n >= (int)(sizeof(uint64_t) * 4)) {
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
                state->H = 0ULL;
                state->K1 = 0ULL;
                state->K2 = 0ULL;
                state->K3 = 0ULL;
                state->K4 = 0ULL;
                ui_cmd_reset_key_mute_state(opts, state);
            }
            return 1;
        }
        default: return 0;
    }
}

static int
apply_cmd_key_management_stream_keys(dsd_state* state, const struct UiCmd* c) {
    if (!state || !c) {
        return 0;
    }
    switch (c->id) {
        case UI_CMD_KEY_TYT_AP_SET: {
            char s[256];
            if (ui_cmd_copy_payload_string(c, s, sizeof s)) {
                tyt_ap_pc4_keystream_creation(state, s);
            }
            return 1;
        }
        case UI_CMD_KEY_RETEVIS_RC2_SET: {
            char s[256];
            if (ui_cmd_copy_payload_string(c, s, sizeof s)) {
                retevis_rc2_keystream_creation(state, s);
            }
            return 1;
        }
        case UI_CMD_KEY_TYT_EP_SET: {
            char s[256];
            if (ui_cmd_copy_payload_string(c, s, sizeof s)) {
                tyt_ep_aes_keystream_creation(state, s);
            }
            return 1;
        }
        case UI_CMD_KEY_KEN_SCR_SET: {
            char s[128];
            if (ui_cmd_copy_payload_string(c, s, sizeof s)) {
                ken_dmr_scrambler_keystream_creation(state, s);
            }
            return 1;
        }
        case UI_CMD_KEY_ANYTONE_BP_SET: {
            char s[128];
            if (ui_cmd_copy_payload_string(c, s, sizeof s)) {
                anytone_bp_keystream_creation(state, s);
            }
            return 1;
        }
        case UI_CMD_KEY_XOR_SET: {
            char s[256];
            if (ui_cmd_copy_payload_string(c, s, sizeof s)) {
                straight_mod_xor_keystream_creation(state, s);
            }
            return 1;
        }
        default: return 0;
    }
}

static int
apply_cmd_key_management_m17(dsd_state* state, const struct UiCmd* c) {
    if (!state || !c || c->id != UI_CMD_M17_USER_DATA_SET) {
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
apply_cmd_key_management(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    if (!c) {
        return 0;
    }
    if (apply_cmd_key_management_basic(opts, state, c)) {
        return 1;
    }
    if (apply_cmd_key_management_block_keys(opts, state, c)) {
        return 1;
    }
    if (apply_cmd_key_management_stream_keys(state, c)) {
        return 1;
    }
    return apply_cmd_key_management_m17(state, c);
}

#ifdef USE_RADIO
static void
ui_dsp_toggle_triplet_op(int op) {
    int cq = 0;
    int f = 0;
    int t = 0;
    rtl_stream_dsp_get(&cq, &f, &t);
    switch (op) {
        case UI_DSP_OP_TOGGLE_CQ: rtl_stream_toggle_cqpsk(cq ? 0 : 1); return;
        case UI_DSP_OP_TOGGLE_FLL: rtl_stream_toggle_fll(f ? 0 : 1); return;
        case UI_DSP_OP_TOGGLE_TED: rtl_stream_toggle_ted(t ? 0 : 1); return;
        default: return;
    }
}

static int
apply_dsp_op_triplet_toggles(const UiDspPayload* p) {
    if (!p) {
        return 0;
    }
    switch (p->op) {
        case UI_DSP_OP_TOGGLE_CQ:
        case UI_DSP_OP_TOGGLE_FLL:
        case UI_DSP_OP_TOGGLE_TED: ui_dsp_toggle_triplet_op(p->op); return 1;
        default: return 0;
    }
}

static int
apply_dsp_op_iq_and_ted(const UiDspPayload* p) {
    if (!p) {
        return 0;
    }
    switch (p->op) {
        case UI_DSP_OP_TOGGLE_IQBAL: {
            int on = rtl_stream_get_iq_balance();
            int new_on = on ? 0 : 1;
            rtl_stream_toggle_iq_balance(new_on);
            dsd_setenv("DSD_NEO_IQ_BALANCE", new_on ? "1" : "0", 1);
            return 1;
        }
        case UI_DSP_OP_IQ_DC_TOGGLE: {
            int k = 0;
            int on = rtl_stream_get_iq_dc(&k);
            int new_on = on ? 0 : 1;
            rtl_stream_set_iq_dc(new_on, -1);
            dsd_setenv("DSD_NEO_IQ_DC_BLOCK", new_on ? "1" : "0", 1);
            return 1;
        }
        case UI_DSP_OP_IQ_DC_K_DELTA: {
            int k = 0;
            (void)rtl_stream_get_iq_dc(&k);
            rtl_stream_set_iq_dc(-1, k + p->a);
            return 1;
        }
        case UI_DSP_OP_TED_GAIN_SET: {
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
apply_dsp_op_clock_and_fm_basic(const UiDspPayload* p) {
    if (!p) {
        return 0;
    }
    switch (p->op) {
        case UI_DSP_OP_C4FM_CLK_CYCLE: {
            int mode = rtl_stream_get_c4fm_clk();
            rtl_stream_set_c4fm_clk((mode + 1) % 3);
            return 1;
        }
        case UI_DSP_OP_C4FM_CLK_SYNC_TOGGLE: {
            int en = rtl_stream_get_c4fm_clk_sync();
            rtl_stream_set_c4fm_clk_sync(en ? 0 : 1);
            return 1;
        }
        case UI_DSP_OP_FM_AGC_TOGGLE: {
            int on = rtl_stream_get_fm_agc();
            rtl_stream_set_fm_agc(on ? 0 : 1);
            return 1;
        }
        case UI_DSP_OP_FM_LIMITER_TOGGLE: {
            int on = rtl_stream_get_fm_limiter();
            rtl_stream_set_fm_limiter(on ? 0 : 1);
            return 1;
        }
        default: return 0;
    }
}

static int
apply_dsp_op_fm_agc_delta(const UiDspPayload* p) {
    if (!p) {
        return 0;
    }
    switch (p->op) {
        case UI_DSP_OP_FM_AGC_TARGET_DELTA: {
            float tgt = 0.0f;
            rtl_stream_get_fm_agc_params(&tgt, NULL, NULL, NULL);
            float nt = tgt + ((float)p->a * 0.01f);
            if (nt < 0.05f) {
                nt = 0.05f;
            }
            if (nt > 2.5f) {
                nt = 2.5f;
            }
            rtl_stream_set_fm_agc_params(nt, -1.0f, -1.0f, -1.0f);
            return 1;
        }
        case UI_DSP_OP_FM_AGC_MIN_DELTA: {
            float mn = 0.0f;
            rtl_stream_get_fm_agc_params(NULL, &mn, NULL, NULL);
            float nm = mn + ((float)p->a * 0.01f);
            if (nm < 0.0f) {
                nm = 0.0f;
            }
            if (nm > 1.0f) {
                nm = 1.0f;
            }
            rtl_stream_set_fm_agc_params(-1.0f, nm, -1.0f, -1.0f);
            return 1;
        }
        case UI_DSP_OP_FM_AGC_ATTACK_DELTA: {
            float au = 0.0f;
            rtl_stream_get_fm_agc_params(NULL, NULL, &au, NULL);
            float na = au + ((float)p->a * 0.01f);
            if (na < 0.0f) {
                na = 0.0f;
            }
            if (na > 1.0f) {
                na = 1.0f;
            }
            rtl_stream_set_fm_agc_params(-1.0f, -1.0f, na, -1.0f);
            return 1;
        }
        case UI_DSP_OP_FM_AGC_DECAY_DELTA: {
            float ad = 0.0f;
            rtl_stream_get_fm_agc_params(NULL, NULL, NULL, &ad);
            float nd = ad + ((float)p->a * 0.01f);
            if (nd < 0.0f) {
                nd = 0.0f;
            }
            if (nd > 1.0f) {
                nd = 1.0f;
            }
            rtl_stream_set_fm_agc_params(-1.0f, -1.0f, -1.0f, nd);
            return 1;
        }
        default: return 0;
    }
}

static int
apply_dsp_op_clock_and_fm(const UiDspPayload* p) {
    if (apply_dsp_op_clock_and_fm_basic(p)) {
        return 1;
    }
    return apply_dsp_op_fm_agc_delta(p);
}

static int
apply_dsp_op_frontend_gain(const UiDspPayload* p) {
    if (!p) {
        return 0;
    }
    switch (p->op) {
        case UI_DSP_OP_TUNER_AUTOGAIN_TOGGLE: {
            int on = rtl_stream_get_tuner_autogain();
            rtl_stream_set_tuner_autogain(on ? 0 : 1);
            return 1;
        }
        default: return 0;
    }
}

static void
apply_dsp_op(const UiDspPayload* p) {
    if (!p) {
        return;
    }
    if (apply_dsp_op_triplet_toggles(p)) {
        return;
    }
    if (apply_dsp_op_iq_and_ted(p)) {
        return;
    }
    if (apply_dsp_op_clock_and_fm(p)) {
        return;
    }
    (void)apply_dsp_op_frontend_gain(p);
}
#endif

static int
apply_cmd_runtime_toggles(dsd_opts* opts, const struct UiCmd* c) {
    if (!opts || !c) {
        return 0;
    }
    switch (c->id) {
        case UI_CMD_DMR_LE_TOGGLE: svc_toggle_dmr_le(opts); return 1;
        case UI_CMD_ALL_MUTES_TOGGLE: svc_toggle_all_mutes(opts); return 1;
        case UI_CMD_INV_X2_TOGGLE: svc_toggle_inv_x2(opts); return 1;
        case UI_CMD_INV_DMR_TOGGLE: svc_toggle_inv_dmr(opts); return 1;
        case UI_CMD_INV_DPMR_TOGGLE: svc_toggle_inv_dpmr(opts); return 1;
        case UI_CMD_INV_M17_TOGGLE: svc_toggle_inv_m17(opts); return 1;
        default: return 0;
    }
}

static int
ui_cmd_handle_wav_static_open(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    if (state && c->n > 0) {
        char path[1024] = {0};
        if (ui_cmd_copy_payload_string(c, path, sizeof path)) {
            int rc = svc_open_static_wav(opts, state, path);
            if (rc == 0) {
                ui_set_toast(state, 3, "Applied: Static WAV output -> %s", path);
            } else {
                ui_set_toast(state, 4, "Failed: Static WAV open -> %s", path);
            }
        }
    }
    return 1;
}

static int
ui_cmd_handle_wav_raw_open(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    if (state && c->n > 0) {
        char path[1024] = {0};
        if (ui_cmd_copy_payload_string(c, path, sizeof path)) {
            int rc = svc_open_raw_wav(opts, state, path);
            if (rc == 0) {
                ui_set_toast(state, 3, "Applied: Raw WAV output -> %s", path);
            } else {
                ui_set_toast(state, 4, "Failed: Raw WAV open -> %s", path);
            }
        }
    }
    return 1;
}

static int
ui_cmd_handle_dsp_out_set(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    if (c->n > 0) {
        char name[256] = {0};
        if (ui_cmd_copy_payload_string(c, name, sizeof name)) {
            int rc = svc_set_dsp_output_file(opts, name);
            if (rc == 0) {
                ui_set_toast(state, 3, "Applied: DSP output -> %s", opts->dsp_out_file);
            } else {
                ui_set_toast(state, 4, "Failed: DSP output path invalid");
            }
        }
    }
    return 1;
}

static int
apply_cmd_io_and_import_file_outputs_a(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    static const struct UiCmdHandlerEntry k_handlers[] = {
        {UI_CMD_WAV_STATIC_OPEN, ui_cmd_handle_wav_static_open},
        {UI_CMD_WAV_RAW_OPEN, ui_cmd_handle_wav_raw_open},
        {UI_CMD_DSP_OUT_SET, ui_cmd_handle_dsp_out_set},
    };
    if (!opts || !c) {
        return 0;
    }
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

static int
ui_cmd_handle_symcap_open(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    if (state && c->n > 0) {
        char path[1024] = {0};
        if (ui_cmd_copy_payload_string(c, path, sizeof path)) {
            int rc = svc_open_symbol_out(opts, state, path);
            if (rc == 0) {
                ui_set_toast(state, 3, "Applied: Symbol capture -> %s", path);
            } else {
                ui_set_toast(state, 4, "Failed: Symbol capture open -> %s", path);
            }
        }
    }
    return 1;
}

static int
ui_cmd_handle_symbol_in_open(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    if (state && c->n > 0) {
        char path[1024] = {0};
        if (ui_cmd_copy_payload_string(c, path, sizeof path)) {
            int rc = svc_open_symbol_in(opts, state, path);
            if (rc == 0) {
                ui_set_toast(state, 3, "Applied: Symbol input -> %s", path);
            } else {
                ui_set_toast(state, 4, "Failed: Symbol input open -> %s", path);
            }
        }
    }
    return 1;
}

static int
ui_cmd_handle_input_wav_set(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    if (c->n > 0 && ui_cmd_copy_payload_string(c, opts->audio_in_dev, sizeof opts->audio_in_dev)) {
        opts->audio_in_type = AUDIO_IN_WAV;
        ui_set_toast(state, 3, "Applied: WAV input -> %s", opts->audio_in_dev);
    }
    return 1;
}

static int
ui_cmd_handle_input_sym_stream_set(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    if (c->n > 0 && ui_cmd_copy_payload_string(c, opts->audio_in_dev, sizeof opts->audio_in_dev)) {
        opts->audio_in_type = AUDIO_IN_SYMBOL_FLT;
        ui_set_toast(state, 3, "Applied: Symbol stream input -> %s", opts->audio_in_dev);
    }
    return 1;
}

static int
ui_cmd_handle_input_set_pulse(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    (void)c;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "pulse");
    opts->audio_in_type = AUDIO_IN_PULSE;
    ui_set_toast(state, 3, "Applied: Input switched to Pulse");
    return 1;
}

static int
apply_cmd_io_and_import_file_outputs_b(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    static const struct UiCmdHandlerEntry k_handlers[] = {
        {UI_CMD_SYMCAP_OPEN, ui_cmd_handle_symcap_open},
        {UI_CMD_SYMBOL_IN_OPEN, ui_cmd_handle_symbol_in_open},
        {UI_CMD_INPUT_WAV_SET, ui_cmd_handle_input_wav_set},
        {UI_CMD_INPUT_SYM_STREAM_SET, ui_cmd_handle_input_sym_stream_set},
        {UI_CMD_INPUT_SET_PULSE, ui_cmd_handle_input_set_pulse},
    };
    if (!opts || !c) {
        return 0;
    }
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

static int
ui_cmd_parse_host_port_payload(const struct UiCmd* c, char* host, size_t host_sz, int32_t* port) {
    if (!c || !host || host_sz == 0 || !port || c->n < (int)(256 + sizeof(int32_t))) {
        return 0;
    }
    DSD_MEMSET(host, 0, host_sz);
    DSD_MEMCPY(host, c->data, (host_sz > 255) ? 255 : (host_sz - 1));
    DSD_MEMCPY(port, c->data + 256, sizeof *port);
    return 1;
}

static int
ui_cmd_handle_udp_out_cfg(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    char host[256] = {0};
    int32_t port = 0;
    if (state && ui_cmd_parse_host_port_payload(c, host, sizeof host, &port)) {
        int rc = svc_udp_output_config(opts, state, host, port);
        if (rc == 0) {
            ui_set_toast(state, 3, "UDP output configured: %s:%d", host, (int)port);
        } else {
            ui_set_toast(state, 4, "UDP output failed: %s:%d", host, (int)port);
        }
    }
    return 1;
}

static int
ui_cmd_handle_tcp_connect_audio_cfg(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    char host[256] = {0};
    int32_t port = 0;
    if (state && ui_cmd_parse_host_port_payload(c, host, sizeof host, &port)) {
        int rc = svc_tcp_connect_audio(opts, host, port);
        if (rc == 0) {
            ui_set_toast(state, 3, "TCP audio connected: %s:%d", host, (int)port);
        } else {
            ui_set_toast(state, 4, "TCP audio connect failed: %s:%d", host, (int)port);
        }
    }
    return 1;
}

static int
ui_cmd_handle_rigctl_connect_cfg(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    char host[256] = {0};
    int32_t port = 0;
    if (state && ui_cmd_parse_host_port_payload(c, host, sizeof host, &port)) {
        int rc = svc_rigctl_connect(opts, host, port);
        if (rc == 0) {
            ui_set_toast(state, 3, "Rigctl connected: %s:%d", host, (int)port);
        } else {
            ui_set_toast(state, 4, "Rigctl connect failed: %s:%d", host, (int)port);
        }
    }
    return 1;
}

static int
ui_cmd_handle_udp_input_cfg(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    char bind[256] = {0};
    int32_t port = 0;
    if (state && ui_cmd_parse_host_port_payload(c, bind, sizeof bind, &port)) {
        DSD_SNPRINTF(opts->udp_in_bindaddr, sizeof opts->udp_in_bindaddr, "%s", bind);
        opts->udp_in_portno = port;
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "udp");
        opts->audio_in_type = AUDIO_IN_UDP;
        ui_set_toast(state, 3, "UDP input set: %s:%d", bind[0] ? bind : "127.0.0.1", (int)port);
    }
    return 1;
}

static int
apply_cmd_io_and_import_network(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    static const struct UiCmdHandlerEntry k_handlers[] = {
        {UI_CMD_UDP_OUT_CFG, ui_cmd_handle_udp_out_cfg},
        {UI_CMD_TCP_CONNECT_AUDIO_CFG, ui_cmd_handle_tcp_connect_audio_cfg},
        {UI_CMD_RIGCTL_CONNECT_CFG, ui_cmd_handle_rigctl_connect_cfg},
        {UI_CMD_UDP_INPUT_CFG, ui_cmd_handle_udp_input_cfg},
    };
    if (!opts || !c) {
        return 0;
    }
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

static int
ui_cmd_parse_i32_payload(const struct UiCmd* c, int32_t* out) {
    if (!c || !out || c->n < (int)sizeof(*out)) {
        return 0;
    }
    DSD_MEMCPY(out, c->data, sizeof *out);
    return 1;
}

static int
ui_cmd_parse_double_payload(const struct UiCmd* c, double* out) {
    if (!c || !out || c->n < (int)sizeof(*out)) {
        return 0;
    }
    DSD_MEMCPY(out, c->data, sizeof *out);
    return 1;
}

#ifdef USE_RADIO
static int
ui_cmd_handle_rtl_enable_input(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    (void)c;
    if (state) {
        int rc = svc_rtl_enable_input(opts, state);
        if (rc == 0) {
            ui_set_toast(state, 3, "Applied: RTL input enabled");
        } else if (ui_rc_is_not_supported(rc)) {
            ui_set_toast(state, 3, "Unsupported: active radio backend cannot enable RTL input");
        } else {
            ui_set_toast(state, 4, "Failed: RTL input enable");
        }
    }
    return 1;
}

static int
ui_cmd_handle_rtl_restart(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    (void)c;
    if (state) {
        int rc = svc_rtl_restart(opts, state);
        if (rc == 0) {
            ui_set_toast(state, 3, "Applied: RTL stream restarted");
        } else if (ui_rc_is_not_supported(rc)) {
            ui_set_toast(state, 3, "Unsupported: active radio backend cannot restart stream");
        } else {
            ui_set_toast(state, 4, "Failed: RTL stream restart");
        }
    }
    return 1;
}

static int
ui_cmd_handle_rtl_set_dev(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    int32_t v = 0;
    if (state && ui_cmd_parse_i32_payload(c, &v)) {
        int rc = svc_rtl_set_dev_index(opts, state, v);
        if (rc == 0) {
            ui_set_toast(state, 3, "Applied: RTL device index -> %d", (int)v);
        } else if (ui_rc_is_not_supported(rc)) {
            ui_set_toast(state, 3, "Unsupported: device index is not available for current radio source");
        } else {
            ui_set_toast(state, 4, "Failed: RTL device index -> %d", (int)v);
        }
    }
    return 1;
}

static int
apply_cmd_io_and_import_rtl_a(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    static const struct UiCmdHandlerEntry k_handlers[] = {
        {UI_CMD_RTL_ENABLE_INPUT, ui_cmd_handle_rtl_enable_input},
        {UI_CMD_RTL_RESTART, ui_cmd_handle_rtl_restart},
        {UI_CMD_RTL_SET_DEV, ui_cmd_handle_rtl_set_dev},
    };
    if (!opts || !c) {
        return 0;
    }
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

static int
ui_cmd_handle_rtl_set_freq(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    int32_t v = 0;
    if (state && ui_cmd_parse_i32_payload(c, &v)) {
        int rc = svc_rtl_set_freq(opts, state, (uint32_t)v);
        if (rc == 0) {
            ui_set_toast(state, 3, "Applied: RTL frequency -> %d Hz", (int)v);
        } else if (ui_rc_is_not_supported(rc)) {
            ui_set_toast(state, 3, "Unsupported: frequency control not available on active backend");
        } else {
            ui_set_toast(state, 4, "Failed: RTL frequency -> %d Hz", (int)v);
        }
    }
    return 1;
}

static int
ui_cmd_handle_rtl_set_gain(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    int32_t v = 0;
    if (state && ui_cmd_parse_i32_payload(c, &v)) {
        int rc = svc_rtl_set_gain(opts, state, v);
        if (rc == 0) {
            ui_set_toast(state, 3, "Applied: RTL gain -> %d", (int)v);
        } else if (ui_rc_is_not_supported(rc)) {
            ui_set_toast(state, 3, "Unsupported: gain control not available on active backend");
        } else {
            ui_set_toast(state, 4, "Failed: RTL gain -> %d", (int)v);
        }
    }
    return 1;
}

static int
ui_cmd_handle_rtl_set_ppm(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    int32_t v = 0;
    if (state && ui_cmd_parse_i32_payload(c, &v)) {
        int rc = svc_rtl_set_ppm(opts, v);
        if (rc == 0) {
            ui_set_toast(state, 3, "Applied: RTL PPM -> %d", rtl_stream_get_requested_ppm(opts));
        } else if (ui_rc_is_not_supported(rc)) {
            ui_set_toast(state, 3, "Unsupported: PPM correction not available on active backend");
        } else {
            ui_set_toast(state, 4, "Failed: RTL PPM update");
        }
    }
    return 1;
}

static int
apply_cmd_io_and_import_rtl_b(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    static const struct UiCmdHandlerEntry k_handlers[] = {
        {UI_CMD_RTL_SET_FREQ, ui_cmd_handle_rtl_set_freq},
        {UI_CMD_RTL_SET_GAIN, ui_cmd_handle_rtl_set_gain},
        {UI_CMD_RTL_SET_PPM, ui_cmd_handle_rtl_set_ppm},
    };
    if (!opts || !c) {
        return 0;
    }
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

static int
ui_cmd_handle_rtl_set_bw(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    int32_t v = 0;
    if (state && ui_cmd_parse_i32_payload(c, &v)) {
        int rc = svc_rtl_set_bandwidth(opts, state, v);
        if (rc == 0) {
            ui_set_toast(state, 3, "Applied: RTL DSP BW -> %d kHz", (int)opts->rtl_dsp_bw_khz);
        } else if (ui_rc_is_not_supported(rc)) {
            ui_set_toast(state, 3, "Unsupported: bandwidth control not available on active backend");
        } else {
            ui_set_toast(state, 4, "Failed: RTL DSP BW update");
        }
    }
    return 1;
}

static int
ui_cmd_handle_rtl_set_sql_db(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    double d = 0.0;
    if (state && ui_cmd_parse_double_payload(c, &d)) {
        int rc = svc_rtl_set_sql_db(opts, d);
        if (rc == 0) {
            ui_set_toast(state, 3, "Applied: RTL squelch -> %.1f dB", d);
        } else if (ui_rc_is_not_supported(rc)) {
            ui_set_toast(state, 3, "Unsupported: squelch control not available on active backend");
        } else {
            ui_set_toast(state, 4, "Failed: RTL squelch update");
        }
    }
    return 1;
}

static int
ui_cmd_handle_rtl_set_vol_mult(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    int32_t v = 0;
    if (state && ui_cmd_parse_i32_payload(c, &v)) {
        int rc = svc_rtl_set_volume_mult(opts, v);
        if (rc == 0) {
            ui_set_toast(state, 3, "Applied: RTL monitor gain -> %dX", (int)opts->rtl_volume_multiplier);
        } else if (ui_rc_is_not_supported(rc)) {
            ui_set_toast(state, 3, "Unsupported: monitor gain not available on active backend");
        } else {
            ui_set_toast(state, 4, "Failed: RTL monitor gain update");
        }
    }
    return 1;
}

static int
apply_cmd_io_and_import_rtl_c(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    static const struct UiCmdHandlerEntry k_handlers[] = {
        {UI_CMD_RTL_SET_BW, ui_cmd_handle_rtl_set_bw},
        {UI_CMD_RTL_SET_SQL_DB, ui_cmd_handle_rtl_set_sql_db},
        {UI_CMD_RTL_SET_VOL_MULT, ui_cmd_handle_rtl_set_vol_mult},
    };
    if (!opts || !c) {
        return 0;
    }
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

static int
ui_cmd_handle_rtl_set_bias_tee(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    int32_t on = 0;
    if (state && ui_cmd_parse_i32_payload(c, &on)) {
        int rc = svc_rtl_set_bias_tee(opts, state, on);
        if (rc == 0) {
            ui_set_toast(state, 3, "Applied: RTL bias tee -> %s", on ? "On" : "Off");
        } else if (ui_rc_is_not_supported(rc)) {
            ui_set_toast(state, 3, "Unsupported: bias tee control not available on active backend");
        } else {
            ui_set_toast(state, 4, "Failed: RTL bias tee update");
        }
    }
    return 1;
}

static int
ui_cmd_handle_rtltcp_set_autotune(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    int32_t on = 0;
    if (state && ui_cmd_parse_i32_payload(c, &on)) {
        int rc = svc_rtltcp_set_autotune(opts, state, on);
        if (rc == 0) {
            ui_set_toast(state, 3, "Applied: RTL-TCP adaptive networking -> %s", on ? "On" : "Off");
        } else if (ui_rc_is_not_supported(rc)) {
            ui_set_toast(state, 3, "Unsupported: RTL-TCP autotune not available on active backend");
        } else {
            ui_set_toast(state, 4, "Failed: RTL-TCP adaptive networking update");
        }
    }
    return 1;
}

static int
ui_cmd_handle_rtl_set_auto_ppm(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    int32_t on = 0;
    if (state && ui_cmd_parse_i32_payload(c, &on)) {
        int rc = svc_rtl_set_auto_ppm(opts, state, on);
        if (rc == 0) {
            ui_set_toast(state, 3, "Applied: Auto PPM -> %s", on ? "On" : "Off");
        } else if (ui_rc_is_not_supported(rc)) {
            ui_set_toast(state, 3, "Unsupported: Auto PPM not available on active backend");
        } else {
            ui_set_toast(state, 4, "Failed: Auto PPM update");
        }
    }
    return 1;
}

static int
apply_cmd_io_and_import_rtl_d(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    static const struct UiCmdHandlerEntry k_handlers[] = {
        {UI_CMD_RTL_SET_BIAS_TEE, ui_cmd_handle_rtl_set_bias_tee},
        {UI_CMD_RTLTCP_SET_AUTOTUNE, ui_cmd_handle_rtltcp_set_autotune},
        {UI_CMD_RTL_SET_AUTO_PPM, ui_cmd_handle_rtl_set_auto_ppm},
    };
    if (!opts || !c) {
        return 0;
    }
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}
#endif

static int
ui_cmd_parse_u32_payload(const struct UiCmd* c, uint32_t* out) {
    if (!c || !out || c->n < (int)sizeof(*out)) {
        return 0;
    }
    DSD_MEMCPY(out, c->data, sizeof *out);
    return 1;
}

static int
ui_cmd_handle_rigctl_set_mod_bw(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    int32_t hz = 0;
    if (state && ui_cmd_parse_i32_payload(c, &hz)) {
        svc_set_rigctl_setmod_bw(opts, hz);
        ui_set_toast(state, 3, "Applied: Rigctl setmod BW -> %d Hz", opts->setmod_bw);
    }
    return 1;
}

static int
ui_cmd_handle_tg_hold_set(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    uint32_t tg = 0;
    (void)opts;
    if (state && ui_cmd_parse_u32_payload(c, &tg)) {
        svc_set_tg_hold(state, tg);
        ui_set_toast(state, 3, "Applied: TG Hold -> %u", tg);
    }
    return 1;
}

static int
ui_cmd_handle_hangtime_set(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    double s = 0.0;
    if (state && ui_cmd_parse_double_payload(c, &s)) {
        svc_set_hangtime(opts, s);
        ui_set_toast(state, 3, "Applied: Hangtime -> %.3f s", opts->trunk_hangtime);
    }
    return 1;
}

static int
ui_cmd_handle_slot_pref_set(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    int32_t p = 0;
    if (state && ui_cmd_parse_i32_payload(c, &p)) {
        svc_set_slot_pref(opts, p);
        ui_set_toast(state, 3, "Applied: Slot preference -> %d", opts->slot_preference + 1);
    }
    return 1;
}

static int
ui_cmd_handle_slots_onoff_set(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    int32_t m = 0;
    if (state && ui_cmd_parse_i32_payload(c, &m)) {
        svc_set_slots_onoff(opts, m);
        ui_set_toast(state, 3, "Applied: Slot mask -> %d", (opts->slot1_on ? 1 : 0) | (opts->slot2_on ? 2 : 0));
    }
    return 1;
}

static int
apply_cmd_io_and_import_runtime_a(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    static const struct UiCmdHandlerEntry k_handlers[] = {
        {UI_CMD_RIGCTL_SET_MOD_BW, ui_cmd_handle_rigctl_set_mod_bw},
        {UI_CMD_TG_HOLD_SET, ui_cmd_handle_tg_hold_set},
        {UI_CMD_HANGTIME_SET, ui_cmd_handle_hangtime_set},
        {UI_CMD_SLOT_PREF_SET, ui_cmd_handle_slot_pref_set},
        {UI_CMD_SLOTS_ONOFF_SET, ui_cmd_handle_slots_onoff_set},
    };
    if (!opts || !c) {
        return 0;
    }
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

static int
apply_cmd_io_and_import_pulse_io(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    if (!opts || !c) {
        return 0;
    }
    switch (c->id) {
        case UI_CMD_PULSE_OUT_SET: {
            if (state && c->n > 0) {
                char name[256] = {0};
                size_t n = c->n < sizeof(name) ? c->n : sizeof(name) - 1;
                DSD_MEMCPY(name, c->data, n);
                name[n] = '\0';
                int rc = svc_set_pulse_output(opts, name);
                if (rc == 0) {
                    ui_set_toast(state, 3, "Applied: Pulse output -> %s", name);
                } else {
                    ui_set_toast(state, 4, "Failed: Pulse output -> %s", name);
                }
            }
            return 1;
        }
        case UI_CMD_PULSE_IN_SET: {
            if (state && c->n > 0) {
                char name[256] = {0};
                size_t n = c->n < sizeof(name) ? c->n : sizeof(name) - 1;
                DSD_MEMCPY(name, c->data, n);
                name[n] = '\0';
                int rc = svc_set_pulse_input(opts, name);
                if (rc == 0) {
                    ui_set_toast(state, 3, "Applied: Pulse input -> %s", name);
                } else {
                    ui_set_toast(state, 4, "Failed: Pulse input -> %s", name);
                }
            }
            return 1;
        }
        default: return 0;
    }
}

static int
ui_cmd_handle_lrrp_set_home(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    (void)c;
    if (state) {
        int rc = svc_lrrp_set_home(opts);
        if (rc == 0) {
            ui_set_toast(state, 3, "Applied: LRRP output -> %s", opts->lrrp_out_file);
        } else {
            ui_set_toast(state, 4, "Failed: LRRP output (home)");
        }
    }
    return 1;
}

static int
ui_cmd_handle_lrrp_set_dsdp(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    (void)c;
    if (state) {
        int rc = svc_lrrp_set_dsdp(opts);
        if (rc == 0) {
            ui_set_toast(state, 3, "Applied: LRRP output -> %s", opts->lrrp_out_file);
        } else {
            ui_set_toast(state, 4, "Failed: LRRP output (DSDPlus)");
        }
    }
    return 1;
}

static int
ui_cmd_handle_lrrp_set_custom(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    if (state && c->n > 0) {
        char path[1024] = {0};
        if (ui_cmd_copy_payload_string(c, path, sizeof path)) {
            int rc = svc_lrrp_set_custom(opts, path);
            if (rc == 0) {
                ui_set_toast(state, 3, "Applied: LRRP output -> %s", path);
            } else {
                ui_set_toast(state, 4, "Failed: LRRP output -> %s", path);
            }
        }
    }
    return 1;
}

static int
ui_cmd_handle_lrrp_disable(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    (void)c;
    if (state) {
        svc_lrrp_disable(opts);
        ui_set_toast(state, 3, "Applied: LRRP output disabled");
    }
    return 1;
}

static int
ui_cmd_handle_p25_p2_params_set(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
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
apply_cmd_io_and_import_lrrp_and_p2(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    static const struct UiCmdHandlerEntry k_handlers[] = {
        {UI_CMD_LRRP_SET_HOME, ui_cmd_handle_lrrp_set_home},
        {UI_CMD_LRRP_SET_DSDP, ui_cmd_handle_lrrp_set_dsdp},
        {UI_CMD_LRRP_SET_CUSTOM, ui_cmd_handle_lrrp_set_custom},
        {UI_CMD_LRRP_DISABLE, ui_cmd_handle_lrrp_disable},
        {UI_CMD_P25_P2_PARAMS_SET, ui_cmd_handle_p25_p2_params_set},
    };
    if (!opts || !c) {
        return 0;
    }
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

static int
ui_cmd_handle_import_channel_map(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    if (state && c->n > 0) {
        char path[1024] = {0};
        if (ui_cmd_copy_payload_string(c, path, sizeof path)) {
            int rc = svc_import_channel_map(opts, state, path);
            if (rc == 0) {
                ui_set_toast(state, 3, "Applied: Channel map imported -> %s", path);
            } else {
                ui_set_toast(state, 4, "Failed: Channel map import -> %s", path);
            }
        }
    }
    return 1;
}

static int
ui_cmd_handle_import_group_list(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    if (state && c->n > 0) {
        char path[1024] = {0};
        if (ui_cmd_copy_payload_string(c, path, sizeof path)) {
            int rc = svc_import_group_list(opts, state, path);
            if (rc == 0) {
                ui_set_toast(state, 3, "Applied: Group list reloaded -> %s", path);
            } else {
                ui_set_toast(state, 4, "Failed: Group list reload -> %s", path);
            }
        }
    }
    return 1;
}

static int
ui_cmd_handle_import_keys_dec(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    if (state && c->n > 0) {
        char path[1024] = {0};
        if (ui_cmd_copy_payload_string(c, path, sizeof path)) {
            int rc = svc_import_keys_dec(opts, state, path);
            if (rc == 0) {
                ui_set_toast(state, 3, "Applied: Keys (DEC) imported -> %s", path);
            } else {
                ui_set_toast(state, 4, "Failed: Keys (DEC) import -> %s", path);
            }
        }
    }
    return 1;
}

static int
ui_cmd_handle_import_keys_hex(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    if (state && c->n > 0) {
        char path[1024] = {0};
        if (ui_cmd_copy_payload_string(c, path, sizeof path)) {
            int rc = svc_import_keys_hex(opts, state, path);
            if (rc == 0) {
                ui_set_toast(state, 3, "Applied: Keys (HEX) imported -> %s", path);
            } else {
                ui_set_toast(state, 4, "Failed: Keys (HEX) import -> %s", path);
            }
        }
    }
    return 1;
}

static int
apply_cmd_io_and_import_imports(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    static const struct UiCmdHandlerEntry k_handlers[] = {
        {UI_CMD_IMPORT_CHANNEL_MAP, ui_cmd_handle_import_channel_map},
        {UI_CMD_IMPORT_GROUP_LIST, ui_cmd_handle_import_group_list},
        {UI_CMD_IMPORT_KEYS_DEC, ui_cmd_handle_import_keys_dec},
        {UI_CMD_IMPORT_KEYS_HEX, ui_cmd_handle_import_keys_hex},
    };
    if (!opts || !c) {
        return 0;
    }
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

static int
apply_cmd_io_and_import(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    if (!opts || !c) {
        return 0;
    }
    if (apply_cmd_io_and_import_file_outputs_a(opts, state, c)) {
        return 1;
    }
    if (apply_cmd_io_and_import_file_outputs_b(opts, state, c)) {
        return 1;
    }
    if (apply_cmd_io_and_import_network(opts, state, c)) {
        return 1;
    }
#ifdef USE_RADIO
    if (apply_cmd_io_and_import_rtl_a(opts, state, c)) {
        return 1;
    }
    if (apply_cmd_io_and_import_rtl_b(opts, state, c)) {
        return 1;
    }
    if (apply_cmd_io_and_import_rtl_c(opts, state, c)) {
        return 1;
    }
    if (apply_cmd_io_and_import_rtl_d(opts, state, c)) {
        return 1;
    }
#endif
    if (apply_cmd_io_and_import_runtime_a(opts, state, c)) {
        return 1;
    }
    if (apply_cmd_io_and_import_pulse_io(opts, state, c)) {
        return 1;
    }
    if (apply_cmd_io_and_import_lrrp_and_p2(opts, state, c)) {
        return 1;
    }
    if (apply_cmd_io_and_import_imports(opts, state, c)) {
        return 1;
    }
    return 0;
}

#ifdef USE_RADIO
static int
apply_cmd_dsp(const struct UiCmd* c) {
    if (!c || c->id != UI_CMD_DSP_OP) {
        return 0;
    }
    UiDspPayload p = {0};
    if (c->n >= (int)sizeof(UiDspPayload)) {
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
    DSD_MEMSET(state->nxdn_sacch_frame_segment, 1, sizeof(state->nxdn_sacch_frame_segment));
    DSD_MEMSET(state->nxdn_sacch_frame_segcrc, 1, sizeof(state->nxdn_sacch_frame_segcrc));
    DSD_MEMSET(state->active_channel, 0, sizeof(state->active_channel));
    dmr_reset_blocks(opts, state);
    state->lasttg = state->lasttgR = 0;
    state->lastsrc = state->lastsrcR = 0;
    state->payload_algid = state->payload_algidR = 0;
    state->payload_keyid = state->payload_keyidR = 0;
    state->payload_mi = state->payload_miR = state->payload_miP = state->payload_miN = 0;
    opts->p25_is_tuned = 0;
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

static void
set_cc_symbol_timing(const dsd_opts* opts, dsd_state* state, int fdma_only) {
    int sym_rate = 0;
    if (fdma_only) {
        // In FDMA-only retune paths, keep existing SPS unless CC type is explicitly FDMA.
        if (state->p25_cc_is_tdma != 0) {
            return;
        }
        sym_rate = 4800;
    } else {
        sym_rate = (state->p25_cc_is_tdma == 1) ? 6000 : 4800;
    }
    state->samplesPerSymbol = dsd_opts_compute_sps_rate(opts, sym_rate, current_demod_rate(opts, state));
    state->symbolCenter = dsd_opts_symbol_center(state->samplesPerSymbol);
}

static void
mark_cc_sync(dsd_state* state, int include_monotonic) {
    state->last_cc_sync_time = time(NULL);
    if (include_monotonic) {
        state->last_cc_sync_time_m = dsd_time_now_monotonic_s();
    }
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
ui_post_cmd(int cmd_id, const void* payload, size_t payload_sz) {
    if (payload_sz > sizeof(g_q[0].data)) {
        payload_sz = sizeof(g_q[0].data);
    }
    ensure_mu_init();
    dsd_mutex_lock(&g_mu);
    if (q_is_full_unlocked()) {
        // Drop the oldest command (advance head) and warn once per burst
        g_head = (g_head + 1) % UI_CMD_Q_CAP;
        atomic_fetch_add(&g_overflow, 1);
        if (atomic_exchange(&g_overflow_warn_gate, 1) == 0) {
            LOG_WARNING("ui_cmd_queue: overflow; dropping oldest command(s).\n");
        }
    }
    struct UiCmd* c = &g_q[g_tail];
    c->id = cmd_id;
    c->n = payload_sz;
    if (payload_sz && payload) {
        DSD_MEMCPY(c->data, payload, payload_sz);
    }
    g_tail = (g_tail + 1) % UI_CMD_Q_CAP;
    dsd_mutex_unlock(&g_mu);
    return 0;
}

static int
apply_cmd_legacy_basic_a(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    switch (c->id) {
        case UI_CMD_QUIT: exitflag = 1; return 1;
        case UI_CMD_FORCE_PRIV_TOGGLE:
            if (!state) {
                return 1;
            }
            if (state->M == 1 || state->M == 0x21) {
                state->M = 0;
            } else {
                state->M = 1;
            }
            return 1;
        case UI_CMD_FORCE_RC4_TOGGLE:
            if (!state) {
                return 1;
            }
            if (state->M == 1 || state->M == 0x21) {
                state->M = 0;
            } else {
                state->M = 0x21;
            }
            return 1;
        case UI_CMD_TOGGLE_COMPACT: opts->ncurses_compact = opts->ncurses_compact ? 0 : 1; return 1;
        case UI_CMD_HISTORY_CYCLE:
            (void)ui_history_cycle_mode();
            ui_request_redraw();
            return 1;
        default: return 0;
    }
}

static int
apply_cmd_legacy_slot_controls(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    switch (c->id) {
        case UI_CMD_SLOT1_TOGGLE:
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
        case UI_CMD_SLOT2_TOGGLE:
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
        case UI_CMD_SLOT_PREF_CYCLE:
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
ui_cmd_handle_payload_toggle_legacy(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    (void)state;
    (void)c;
    opts->payload = opts->payload ? 0 : 1;
    return 1;
}

static int
ui_cmd_handle_p25_ga_toggle_legacy(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    (void)c;
    opts->show_p25_group_affiliations = opts->show_p25_group_affiliations ? 0 : 1;
    if (state) {
        DSD_SNPRINTF(state->ui_msg, sizeof state->ui_msg, "P25 Group Affiliation: %s",
                     opts->show_p25_group_affiliations ? "On" : "Off");
        state->ui_msg_expire = time(NULL) + 3;
    }
    return 1;
}

static int
ui_cmd_handle_lpf_toggle_legacy(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    (void)state;
    (void)c;
    opts->use_lpf = opts->use_lpf ? 0 : 1;
    return 1;
}

static int
ui_cmd_handle_hpf_toggle_legacy(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    (void)state;
    (void)c;
    opts->use_hpf = opts->use_hpf ? 0 : 1;
    return 1;
}

static int
ui_cmd_handle_pbf_toggle_legacy(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    (void)state;
    (void)c;
    opts->use_pbf = opts->use_pbf ? 0 : 1;
    return 1;
}

static int
ui_cmd_handle_hpf_d_toggle_legacy(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    (void)state;
    (void)c;
    opts->use_hpf_d = opts->use_hpf_d ? 0 : 1;
    return 1;
}

static int
ui_cmd_handle_aggr_sync_toggle_legacy(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    (void)state;
    (void)c;
    opts->aggressive_framesync = opts->aggressive_framesync ? 0 : 1;
    return 1;
}

static int
ui_cmd_handle_call_alert_toggle_legacy(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    (void)state;
    (void)c;
    uint8_t events = dsd_call_alert_mask_events(opts->call_alert_events);
    opts->call_alert_events = events;
    opts->call_alert = opts->call_alert ? 0 : (events ? 1 : 0);
    return 1;
}

static int
ui_cmd_handle_call_alert_events_set_legacy(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
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
apply_cmd_legacy_payload_filters(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    static const struct UiCmdHandlerEntry k_handlers[] = {
        {UI_CMD_PAYLOAD_TOGGLE, ui_cmd_handle_payload_toggle_legacy},
        {UI_CMD_P25_GA_TOGGLE, ui_cmd_handle_p25_ga_toggle_legacy},
        {UI_CMD_LPF_TOGGLE, ui_cmd_handle_lpf_toggle_legacy},
        {UI_CMD_HPF_TOGGLE, ui_cmd_handle_hpf_toggle_legacy},
        {UI_CMD_PBF_TOGGLE, ui_cmd_handle_pbf_toggle_legacy},
        {UI_CMD_HPF_D_TOGGLE, ui_cmd_handle_hpf_d_toggle_legacy},
        {UI_CMD_AGGR_SYNC_TOGGLE, ui_cmd_handle_aggr_sync_toggle_legacy},
        {UI_CMD_CALL_ALERT_TOGGLE, ui_cmd_handle_call_alert_toggle_legacy},
        {UI_CMD_CALL_ALERT_EVENTS_SET, ui_cmd_handle_call_alert_events_set_legacy},
    };
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

static int
apply_cmd_legacy_constellation(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    (void)state;
    switch (c->id) {
        case UI_CMD_CONST_TOGGLE:
            if (opts->audio_in_type == AUDIO_IN_RTL) {
                opts->constellation = opts->constellation ? 0 : 1;
            }
            return 1;
        case UI_CMD_CONST_NORM_TOGGLE:
            if (opts->audio_in_type == AUDIO_IN_RTL && opts->constellation == 1) {
                opts->const_norm_mode = (opts->const_norm_mode == 0) ? 1 : 0;
            }
            return 1;
        case UI_CMD_CONST_GATE_DELTA:
            if (opts->audio_in_type == AUDIO_IN_RTL && opts->constellation == 1) {
                float d = 0.0f;
                if (c->n >= (int)sizeof(float)) {
                    DSD_MEMCPY(&d, c->data, sizeof(float));
                }
                float* g = (opts->mod_qpsk == 1) ? &opts->const_gate_qpsk : &opts->const_gate_other;
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
ui_cmd_handle_eye_toggle_legacy(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    (void)state;
    (void)c;
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        opts->eye_view = opts->eye_view ? 0 : 1;
    }
    return 1;
}

static int
ui_cmd_handle_eye_unicode_toggle_legacy(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    (void)state;
    (void)c;
    if (opts->audio_in_type == AUDIO_IN_RTL && opts->eye_view == 1) {
        opts->eye_unicode = opts->eye_unicode ? 0 : 1;
    }
    return 1;
}

static int
ui_cmd_handle_eye_color_toggle_legacy(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    (void)state;
    (void)c;
    if (opts->audio_in_type == AUDIO_IN_RTL && opts->eye_view == 1) {
        opts->eye_color = opts->eye_color ? 0 : 1;
    }
    return 1;
}

static int
ui_cmd_handle_fsk_hist_toggle_legacy(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    (void)state;
    (void)c;
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        opts->fsk_hist_view = opts->fsk_hist_view ? 0 : 1;
    }
    return 1;
}

static int
ui_cmd_handle_spectrum_toggle_legacy(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    (void)state;
    (void)c;
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        opts->spectrum_view = opts->spectrum_view ? 0 : 1;
    }
    return 1;
}

static int
ui_cmd_handle_spec_size_delta_legacy(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    int32_t d = 0;
    (void)state;
    if (c->n >= (int)sizeof(int32_t)) {
        DSD_MEMCPY(&d, c->data, sizeof(int32_t));
    }
    if (opts->audio_in_type == AUDIO_IN_RTL && opts->spectrum_view == 1) {
        /* Keep legacy toggle state normalized while applying size changes. */
        opts->spectrum_view = 1;
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
apply_cmd_legacy_eye_spectrum(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    static const struct UiCmdHandlerEntry k_handlers[] = {
        {UI_CMD_EYE_TOGGLE, ui_cmd_handle_eye_toggle_legacy},
        {UI_CMD_EYE_UNICODE_TOGGLE, ui_cmd_handle_eye_unicode_toggle_legacy},
        {UI_CMD_EYE_COLOR_TOGGLE, ui_cmd_handle_eye_color_toggle_legacy},
        {UI_CMD_FSK_HIST_TOGGLE, ui_cmd_handle_fsk_hist_toggle_legacy},
        {UI_CMD_SPECTRUM_TOGGLE, ui_cmd_handle_spectrum_toggle_legacy},
        {UI_CMD_SPEC_SIZE_DELTA, ui_cmd_handle_spec_size_delta_legacy},
    };
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

static int
apply_cmd_legacy_trunk_controls(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    switch (c->id) {
        case UI_CMD_DMR_RESET:
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
        case UI_CMD_TCP_CONNECT_AUDIO: {
            int rc = svc_tcp_connect_audio(opts, opts->tcp_hostname, opts->tcp_portno);
            if (rc == 0) {
                LOG_INFO("TCP Socket Connected Successfully.\n");
                ui_set_toast(state, 3, "TCP audio connected: %s:%d", opts->tcp_hostname, opts->tcp_portno);
            } else {
                LOG_ERROR("TCP Socket Connection Error.\n");
                ui_set_toast(state, 4, "TCP audio connect failed: %s:%d", opts->tcp_hostname, opts->tcp_portno);
            }
            return 1;
        }
        case UI_CMD_RIGCTL_CONNECT:
            DSD_MEMCPY(opts->rigctlhostname, opts->tcp_hostname, sizeof(opts->rigctlhostname));
            opts->rigctl_sockfd = Connect(opts->rigctlhostname, opts->rigctlportno);
            opts->use_rigctl = (opts->rigctl_sockfd != 0) ? 1 : 0;
            if (opts->use_rigctl) {
                ui_set_toast(state, 3, "Rigctl connected: %s:%d", opts->rigctlhostname, opts->rigctlportno);
            } else {
                ui_set_toast(state, 4, "Rigctl connect failed: %s:%d", opts->rigctlhostname, opts->rigctlportno);
            }
            return 1;
        case UI_CMD_RETURN_CC:
            if (!state) {
                return 1;
            }
            if (opts->p25_trunk == 1 && (state->trunk_cc_freq != 0 || state->p25_cc_freq != 0)) {
                reset_call_tracking(opts, state, 1);
                io_control_set_freq(opts, state, current_cc_freq(state));
                mark_cc_sync(state, 1);
                set_cc_symbol_timing(opts, state, 0);
                LOG_INFO("User Activated Return to CC\n");
            }
            return 1;
        case UI_CMD_SIM_NOCAR:
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
apply_cmd_legacy_lockout_slot(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    if (!state) {
        return (c && c->id == UI_CMD_LOCKOUT_SLOT) ? 1 : 0;
    }
    if (c->id != UI_CMD_LOCKOUT_SLOT) {
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
        LOG_WARNING("User lockout for TG %d could not be applied (rc=%d); skipping persistence.\n", tg, upsert_rc);
        return 1;
    }

    int eh_slot = (slot == 0) ? 0 : 1;
    DSD_SNPRINTF(state->event_history_s[eh_slot].Event_History_Items[0].internal_str,
                 sizeof state->event_history_s[eh_slot].Event_History_Items[0].internal_str,
                 "Target: %d; has been locked out; User Lock Out.", tg);
    watchdog_event_current(opts, state, eh_slot);
    DSD_SNPRINTF(state->call_string[eh_slot], sizeof state->call_string[eh_slot], "%s",
                 "                     "); // 21 spaces

    DSD_SNPRINTF(metadata, sizeof(metadata), "%02X",
                 (unsigned int)((slot == 0) ? state->payload_algid : state->payload_algidR));
    if (dsd_tg_policy_append_group_file_row(opts, &lockout_entry, metadata) != 0) {
        LOG_WARNING("User lockout for TG %d was applied in-memory but could not be persisted to '%s'.\n", tg,
                    opts->group_in_file);
    }

    reset_call_tracking(opts, state, 1);
    if (opts->p25_trunk == 1) {
        noCarrier(opts, state);
        long f = current_cc_freq(state);
        io_control_set_freq(opts, state, f);
        state->trunk_cc_freq = f;
    }
    mark_cc_sync(state, 0);
    set_cc_symbol_timing(opts, state, 1);
    return 1;
}

static int
apply_cmd_legacy_provoice_m17(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    switch (c->id) {
        case UI_CMD_M17_TX_TOGGLE:
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
        case UI_CMD_PROVOICE_ESK_TOGGLE:
            if (!state) {
                return 1;
            }
            if (opts->frame_provoice == 1) {
                state->esk_mask = (state->esk_mask == 0) ? 0xA0 : 0;
            }
            return 1;
        case UI_CMD_PROVOICE_MODE_TOGGLE:
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
                opts->p25_is_tuned = 0;
                state->lasttg = 0;
                state->lastsrc = 0;
            }
            return 1;
        default: return 0;
    }
}

static int
apply_cmd_legacy_channel_cycle(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    if (c->id != UI_CMD_CHANNEL_CYCLE) {
        return 0;
    }
    if (opts->use_rigctl == 1 || opts->audio_in_type == AUDIO_IN_RTL) {
        reset_call_tracking(opts, state, 0);
        if (opts->p25_prefer_candidates == 1) {
            long cand = 0;
            if (p25_sm_next_cc_candidate(state, &cand)) {
                io_control_set_freq(opts, state, cand);
                LOG_INFO("Candidate Cycle: tuning to %.06lf MHz\n", (double)cand / 1000000);
                mark_cc_sync(state, 1);
                return 1;
            }
        }
        if (state->lcn_freq_roll >= state->lcn_freq_count) {
            state->lcn_freq_roll = 0;
        }
        if (state->lcn_freq_roll != 0
            && state->trunk_lcn_freq[state->lcn_freq_roll - 1] == state->trunk_lcn_freq[state->lcn_freq_roll]) {
            state->lcn_freq_roll++;
            if (state->lcn_freq_roll >= state->lcn_freq_count) {
                state->lcn_freq_roll = 0;
            }
        }
        if (state->trunk_lcn_freq[state->lcn_freq_roll] != 0) {
            io_control_set_freq(opts, state, state->trunk_lcn_freq[state->lcn_freq_roll]);
            LOG_INFO("Channel Cycle: tuning to %.06lf MHz\n",
                     (double)state->trunk_lcn_freq[state->lcn_freq_roll] / 1000000);
        }
        state->lcn_freq_roll++;
        mark_cc_sync(state, 1);
        set_cc_symbol_timing(opts, state, 0);
    }
    return 1;
}

static int
ui_cmd_handle_symcap_save_legacy(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    (void)c;
    char timestr[7];
    char datestr[9];
    getTime_buf(timestr);
    getDate_buf(datestr);
    DSD_SNPRINTF(opts->symbol_out_file, sizeof opts->symbol_out_file, "%s_%s_dibit_capture.bin", datestr, timestr);
    openSymbolOutFile(opts, state);
    if (state && state->event_history_s) {
        state->event_history_s[0].Event_History_Items[0].color_pair = 4;
        char event_str[2000] = {0};
        DSD_SNPRINTF(event_str, sizeof event_str, "DSD-neo Dibit Capture File Started: %s;", opts->symbol_out_file);
        watchdog_event_datacall(opts, state, 0xFFFFFF, 0xFFFFFF, event_str, 0);
        state->lastsrc = 0;
        watchdog_event_history(opts, state, 0);
        watchdog_event_current(opts, state, 0);
    }
    opts->symbol_out_file_creation_time = time(NULL);
    opts->symbol_out_file_is_auto = 1;
    return 1;
}

static int
ui_cmd_handle_symcap_stop_legacy(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    (void)c;
    if (opts->symbol_out_f) {
        closeSymbolOutFile(opts, state);
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", opts->symbol_out_file);
        if (state && state->event_history_s) {
            state->event_history_s[0].Event_History_Items[0].color_pair = 4;
            char event_str[2000] = {0};
            DSD_SNPRINTF(event_str, sizeof event_str, "DSD-neo Dibit Capture File  Closed: %s;", opts->symbol_out_file);
            watchdog_event_datacall(opts, state, 0xFFFFFF, 0xFFFFFF, event_str, 0);
            state->lastsrc = 0;
            watchdog_event_history(opts, state, 0);
            watchdog_event_current(opts, state, 0);
        }
    }
    opts->symbol_out_file_is_auto = 0;
    return 1;
}

static int
ui_cmd_handle_replay_last_legacy(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    dsd_stat_t sb;
    (void)c;
    if (dsd_stat_path(opts->audio_in_dev, &sb) != 0) {
        LOG_ERROR("Error, couldn't open %s\n", opts->audio_in_dev);
        return 1;
    }
    if (dsd_stat_is_regular(&sb)) {
        opts->symbolfile = dsd_fopen_existing_regular_file(opts->audio_in_dev, "rb");
        if (opts->symbolfile) {
            opts->audio_in_type = AUDIO_IN_SYMBOL_BIN;
            state->symbol_replay_format = DSD_SYMBOL_REPLAY_FORMAT_UNKNOWN;
            state->symbol_replay_header_checked = 0;
            state->symbol_replay_has_soft = 0;
        }
    }
    return 1;
}

static int
ui_cmd_handle_wav_start_legacy(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    char wav_file_directory[1024] = {0};
    dsd_stat_t st;
    (void)state;
    (void)c;
    DSD_SNPRINTF(wav_file_directory, sizeof wav_file_directory, "%s", opts->wav_out_dir);
    if (dsd_stat_path(wav_file_directory, &st) == -1) {
        LOG_NOTICE("%s wav file directory does not exist\n", wav_file_directory);
        LOG_NOTICE("Creating directory %s to save decoded wav files\n", wav_file_directory);
        dsd_mkdir(wav_file_directory, 0700);
    }
    LOG_NOTICE("Per Call Wav File Enabled to Directory: %s\n", opts->wav_out_dir);
    opts->wav_out_f = open_wav_file(opts->wav_out_dir, opts->wav_out_file, sizeof opts->wav_out_file, 8000, 0);
    opts->wav_out_fR = open_wav_file(opts->wav_out_dir, opts->wav_out_fileR, sizeof opts->wav_out_fileR, 8000, 0);
    opts->dmr_stereo_wav = 1;
    return 1;
}

static int
ui_cmd_handle_wav_stop_legacy(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
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
ui_cmd_handle_stop_playback_legacy(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    (void)state;
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
        }
    } else {
        opts->audio_in_type = AUDIO_IN_STDIN;
    }
    return 1;
}

static int
apply_cmd_legacy_capture_playback(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    static const struct UiCmdHandlerEntry k_handlers[] = {
        {UI_CMD_SYMCAP_SAVE, ui_cmd_handle_symcap_save_legacy},
        {UI_CMD_SYMCAP_STOP, ui_cmd_handle_symcap_stop_legacy},
        {UI_CMD_REPLAY_LAST, ui_cmd_handle_replay_last_legacy},
        {UI_CMD_WAV_START, ui_cmd_handle_wav_start_legacy},
        {UI_CMD_WAV_STOP, ui_cmd_handle_wav_stop_legacy},
        {UI_CMD_STOP_PLAYBACK, ui_cmd_handle_stop_playback_legacy},
    };
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

static int
apply_cmd_legacy_misc_config(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    switch (c->id) {
        case UI_CMD_CRC_RELAX_TOGGLE: svc_toggle_crc_relax(opts); return 1;
        case UI_CMD_LCW_RETUNE_TOGGLE: svc_toggle_lcw_retune(opts); return 1;
        case UI_CMD_P25_CC_CAND_TOGGLE: opts->p25_prefer_candidates = opts->p25_prefer_candidates ? 0 : 1; return 1;
        case UI_CMD_REVERSE_MUTE_TOGGLE: svc_toggle_reverse_mute(opts); return 1;
        case UI_CMD_CONFIG_APPLY: {
            if (state && c->n >= sizeof(dsdneoUserConfig)) {
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

                DSD_SNPRINTF(old_audio_in_dev, sizeof old_audio_in_dev, "%s", opts->audio_in_dev);
                DSD_SNPRINTF(old_audio_out_dev, sizeof old_audio_out_dev, "%s", opts->audio_out_dev);

                DSD_MEMCPY(&cfg, c->data, sizeof cfg);
                dsd_apply_user_config_to_opts(&cfg, opts, state);
#ifdef USE_RADIO
                apply_cfg_live_rtl_ppm_request(opts, &cfg, old_audio_in_type);
#endif
                apply_cfg_runtime_hot_switches(opts, state, &cfg, old_audio_in_dev, old_audio_in_type,
                                               old_wav_sample_rate, old_effective_input_rate, old_audio_out_dev,
                                               old_audio_out_type);
                restore_live_pcm_rate_after_staged_file_apply(opts, &cfg, old_wav_sample_rate);
                apply_cfg_file_runtime_rate(opts, state, &cfg, old_runtime_input_rate, old_samples_per_symbol,
                                            old_symbol_center, old_jitter);
            }
            return 1;
        }
        default: return 0;
    }
}

static void
apply_cmd(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    static const UiCmdHandlerFn k_legacy_groups[] = {
        apply_cmd_legacy_basic_a,          apply_cmd_legacy_slot_controls, apply_cmd_legacy_payload_filters,
        apply_cmd_legacy_constellation,    apply_cmd_legacy_eye_spectrum,  apply_cmd_legacy_trunk_controls,
        apply_cmd_legacy_lockout_slot,     apply_cmd_legacy_provoice_m17,  apply_cmd_legacy_channel_cycle,
        apply_cmd_legacy_capture_playback, apply_cmd_legacy_misc_config,
    };
    if (!c) {
        return;
    }
    if (!opts) {
        if (c->id == UI_CMD_QUIT) {
            exitflag = 1;
        }
        return;
    }
    if (ui_cmd_dispatch(opts, state, c) || apply_cmd_ui_visibility(opts, c) || apply_cmd_key_management(opts, state, c)
        || apply_cmd_runtime_toggles(opts, c) || apply_cmd_io_and_import(opts, state, c)
#ifdef USE_RADIO
        || apply_cmd_dsp(c)
#endif
    ) {
        return;
    }
    for (size_t i = 0; i < (sizeof k_legacy_groups / sizeof k_legacy_groups[0]); ++i) {
        if (k_legacy_groups[i](opts, state, c)) {
            return;
        }
    }
}

int
ui_drain_cmds(dsd_opts* opts, dsd_state* state) {
    int n_applied = 0;
    ensure_mu_init();
    for (;;) {
        struct UiCmd cmd;
        int have = 0;
        dsd_mutex_lock(&g_mu);
        if (!q_is_empty_unlocked()) {
            cmd = g_q[g_head];
            g_head = (g_head + 1) % UI_CMD_Q_CAP;
            have = 1;
        }
        // Reset overflow warning gate when queue has space again
        if (((g_tail + 1) % UI_CMD_Q_CAP) != g_head) {
            atomic_store(&g_overflow_warn_gate, 0);
        }
        dsd_mutex_unlock(&g_mu);
        if (!have) {
            break;
        }
        apply_cmd(opts, state, &cmd);
        // After applying a command, publish updated snapshots so the UI can
        // render consistent opts/state without racing live structures.
        ui_publish_opts_snapshot(opts);
        if (state) {
            ui_publish_snapshot(state);
        }
        n_applied++;
    }
    return n_applied;
}
