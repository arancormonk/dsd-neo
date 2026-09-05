// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

#include <ctype.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    dsd_scan_settings configured;
    dsd_scan_settings effective;
    dsd_scan_modulation modulation;
    dsd_scan_mode mode;
    dsdneoUserDecodeMode configured_mode;
    int suspended;
    dsd_scan_option_values options;
} scan_scope;

static const char* const mode_names[] = {"", "p25", "dmr", "nxdn96", "nxdn48", "dpmr", "dstar", "ysf", "m17"};
static const dsdneoUserDecodeMode mode_presets[] = {DSDCFG_MODE_AUTO,   DSDCFG_MODE_TDMA,   DSDCFG_MODE_DMR,
                                                    DSDCFG_MODE_NXDN96, DSDCFG_MODE_NXDN48, DSDCFG_MODE_DPMR,
                                                    DSDCFG_MODE_DSTAR,  DSDCFG_MODE_YSF,    DSDCFG_MODE_M17};

const char*
dsd_scan_mode_name(dsd_scan_mode mode) {
    return (unsigned)mode < sizeof(mode_names) / sizeof(mode_names[0]) ? mode_names[mode] : "";
}

int
dsd_scan_mode_parse(const char* text, dsd_scan_mode* mode) {
    if (!mode) {
        return -1;
    }
    if (!text) {
        *mode = DSD_SCAN_MODE_INHERIT;
        return 0;
    }
    while (isspace((unsigned char)*text)) {
        text++;
    }
    size_t len = strlen(text);
    while (len && isspace((unsigned char)text[len - 1])) {
        len--;
    }
    for (size_t i = 0; i < sizeof(mode_names) / sizeof(mode_names[0]); i++) {
        if (strlen(mode_names[i]) != len) {
            continue;
        }
        size_t j = 0;
        while (j < len && tolower((unsigned char)text[j]) == mode_names[i][j]) {
            j++;
        }
        if (j == len) {
            *mode = (dsd_scan_mode)i;
            return 0;
        }
    }
    return -1;
}

static scan_scope*
scan_scope_get(const dsd_state* state) {
    return DSD_STATE_EXT_GET_AS(scan_scope, state, DSD_STATE_EXT_RUNTIME_SCAN_MODE);
}

void
dsd_scan_settings_capture(const dsd_opts* opts, const dsd_state* state, dsd_scan_settings* out) {
    if (!opts || !state || !out) {
        return;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    out->force_key = state->M;
    out->aggressive_framesync = opts->aggressive_framesync;
    out->dmr_crc_relaxed_default = opts->dmr_crc_relaxed_default;
    out->scan_voice_only = opts->scan_voice_only;
    out->scan_voice_qualify_ms = opts->scan_voice_qualify_ms;
    out->scan_voice_hold_ms = opts->scan_voice_hold_ms;
    out->dmr_mute_encL = opts->dmr_mute_encL;
    out->dmr_mute_encR = opts->dmr_mute_encR;
    out->unmute_encrypted_p25 = opts->unmute_encrypted_p25;
    DSD_MEMCPY(out->group_in_file, opts->group_in_file, sizeof(out->group_in_file));
    out->frame_dstar = opts->frame_dstar;
    out->frame_x2tdma = opts->frame_x2tdma;
    out->frame_p25p1 = opts->frame_p25p1;
    out->frame_p25p2 = opts->frame_p25p2;
    out->frame_nxdn48 = opts->frame_nxdn48;
    out->frame_nxdn96 = opts->frame_nxdn96;
    out->frame_dmr = opts->frame_dmr;
    out->frame_dpmr = opts->frame_dpmr;
    out->frame_provoice = opts->frame_provoice;
    out->frame_ysf = opts->frame_ysf;
    out->frame_m17 = opts->frame_m17;
    out->mod_c4fm = opts->mod_c4fm;
    out->mod_qpsk = opts->mod_qpsk;
    out->mod_gfsk = opts->mod_gfsk;
    out->mod_cli_lock = opts->mod_cli_lock;
    out->mod_p25p2_c4fm = opts->mod_p25p2_c4fm;
    out->mod_p25p2_profile_lock = opts->mod_p25p2_profile_lock;
    out->inverted_p2 = opts->inverted_p2;
    out->inverted_x2tdma = opts->inverted_x2tdma;
    out->inverted_dmr = opts->inverted_dmr;
    out->inverted_dpmr = opts->inverted_dpmr;
    out->inverted_ysf = opts->inverted_ysf;
    out->inverted_m17 = opts->inverted_m17;
    out->dmr_stereo = opts->dmr_stereo;
    out->dmr_mono = opts->dmr_mono;
    out->use_cosine_filter = opts->use_cosine_filter;
    out->ssize = opts->ssize;
    out->msize = opts->msize;
    out->analog_only = opts->analog_only;
    out->monitor_input_audio = opts->monitor_input_audio;
    DSD_MEMCPY(out->output_name, opts->output_name, sizeof(out->output_name));
    out->state_rf_mod = state->rf_mod;
    out->state_samplesPerSymbol = state->samplesPerSymbol;
    out->state_symbolCenter = state->symbolCenter;
    out->state_dmr_stereo = state->dmr_stereo;
    out->state_sps_hunt_idx = state->sps_hunt_idx;
}

static void
scan_settings_restore_row_opts(const dsd_scan_settings* saved, dsd_opts* opts) {
    opts->aggressive_framesync = saved->aggressive_framesync;
    opts->dmr_crc_relaxed_default = saved->dmr_crc_relaxed_default;
    opts->scan_voice_only = saved->scan_voice_only;
    opts->scan_voice_qualify_ms = saved->scan_voice_qualify_ms;
    opts->scan_voice_hold_ms = saved->scan_voice_hold_ms;
    opts->dmr_mute_encL = saved->dmr_mute_encL;
    opts->dmr_mute_encR = saved->dmr_mute_encR;
    opts->unmute_encrypted_p25 = saved->unmute_encrypted_p25;
    DSD_MEMCPY(opts->group_in_file, saved->group_in_file, sizeof(opts->group_in_file));
}

static void
scan_settings_restore_opts(const dsd_scan_settings* saved, dsd_opts* opts) {
    scan_settings_restore_row_opts(saved, opts);
    opts->frame_dstar = saved->frame_dstar;
    opts->frame_x2tdma = saved->frame_x2tdma;
    opts->frame_p25p1 = saved->frame_p25p1;
    opts->frame_p25p2 = saved->frame_p25p2;
    opts->frame_nxdn48 = saved->frame_nxdn48;
    opts->frame_nxdn96 = saved->frame_nxdn96;
    opts->frame_dmr = saved->frame_dmr;
    opts->frame_dpmr = saved->frame_dpmr;
    opts->frame_provoice = saved->frame_provoice;
    opts->frame_ysf = saved->frame_ysf;
    opts->frame_m17 = saved->frame_m17;
    opts->mod_c4fm = saved->mod_c4fm;
    opts->mod_qpsk = saved->mod_qpsk;
    opts->mod_gfsk = saved->mod_gfsk;
    opts->mod_cli_lock = saved->mod_cli_lock;
    opts->mod_p25p2_c4fm = saved->mod_p25p2_c4fm;
    opts->mod_p25p2_profile_lock = saved->mod_p25p2_profile_lock;
    opts->inverted_p2 = saved->inverted_p2;
    opts->inverted_x2tdma = saved->inverted_x2tdma;
    opts->inverted_dmr = saved->inverted_dmr;
    opts->inverted_dpmr = saved->inverted_dpmr;
    opts->inverted_ysf = saved->inverted_ysf;
    opts->inverted_m17 = saved->inverted_m17;
    opts->dmr_stereo = saved->dmr_stereo;
    opts->dmr_mono = saved->dmr_mono;
    opts->use_cosine_filter = saved->use_cosine_filter;
    opts->ssize = saved->ssize;
    opts->msize = saved->msize;
    opts->analog_only = saved->analog_only;
    opts->monitor_input_audio = saved->monitor_input_audio;
    DSD_MEMCPY(opts->output_name, saved->output_name, sizeof(opts->output_name));
}

void
dsd_scan_settings_restore(const dsd_scan_settings* saved, dsd_opts* opts, dsd_state* state) {
    if (!saved || !opts || !state) {
        return;
    }
    scan_settings_restore_opts(saved, opts);
    state->M = saved->force_key;
    state->rf_mod = saved->state_rf_mod;
    state->samplesPerSymbol = saved->state_samplesPerSymbol;
    state->symbolCenter = saved->state_symbolCenter;
    state->dmr_stereo = saved->state_dmr_stereo;
    state->sps_hunt_idx = saved->state_sps_hunt_idx;
}

static int
scan_settings_fields_equal(const dsd_scan_settings* a, const dsd_scan_settings* b, const size_t* fields, size_t count) {
    for (size_t i = 0; i < count; i++) {
        const int* av = (const int*)((const unsigned char*)a + fields[i]);
        const int* bv = (const int*)((const unsigned char*)b + fields[i]);
        if (*av != *bv) {
            return 0;
        }
    }
    return 1;
}

int
dsd_scan_settings_equal(const dsd_scan_settings* a, const dsd_scan_settings* b, int include_timing) {
    /* Explicit membership, independent of struct order and padding. */
    static const size_t options[] = {
        offsetof(dsd_scan_settings, force_key),
        offsetof(dsd_scan_settings, aggressive_framesync),
        offsetof(dsd_scan_settings, dmr_crc_relaxed_default),
        offsetof(dsd_scan_settings, scan_voice_only),
        offsetof(dsd_scan_settings, scan_voice_qualify_ms),
        offsetof(dsd_scan_settings, scan_voice_hold_ms),
        offsetof(dsd_scan_settings, dmr_mute_encL),
        offsetof(dsd_scan_settings, dmr_mute_encR),
        offsetof(dsd_scan_settings, unmute_encrypted_p25),
        offsetof(dsd_scan_settings, frame_dstar),
        offsetof(dsd_scan_settings, frame_x2tdma),
        offsetof(dsd_scan_settings, frame_p25p1),
        offsetof(dsd_scan_settings, frame_p25p2),
        offsetof(dsd_scan_settings, frame_nxdn48),
        offsetof(dsd_scan_settings, frame_nxdn96),
        offsetof(dsd_scan_settings, frame_dmr),
        offsetof(dsd_scan_settings, frame_dpmr),
        offsetof(dsd_scan_settings, frame_provoice),
        offsetof(dsd_scan_settings, frame_ysf),
        offsetof(dsd_scan_settings, frame_m17),
        offsetof(dsd_scan_settings, mod_c4fm),
        offsetof(dsd_scan_settings, mod_qpsk),
        offsetof(dsd_scan_settings, mod_gfsk),
        offsetof(dsd_scan_settings, mod_cli_lock),
        offsetof(dsd_scan_settings, mod_p25p2_c4fm),
        offsetof(dsd_scan_settings, mod_p25p2_profile_lock),
        offsetof(dsd_scan_settings, inverted_p2),
        offsetof(dsd_scan_settings, inverted_x2tdma),
        offsetof(dsd_scan_settings, inverted_dmr),
        offsetof(dsd_scan_settings, inverted_dpmr),
        offsetof(dsd_scan_settings, inverted_ysf),
        offsetof(dsd_scan_settings, inverted_m17),
        offsetof(dsd_scan_settings, dmr_stereo),
        offsetof(dsd_scan_settings, dmr_mono),
        offsetof(dsd_scan_settings, use_cosine_filter),
        offsetof(dsd_scan_settings, ssize),
        offsetof(dsd_scan_settings, msize),
        offsetof(dsd_scan_settings, analog_only),
        offsetof(dsd_scan_settings, monitor_input_audio),
    };
    static const size_t timing[] = {
        offsetof(dsd_scan_settings, state_rf_mod),       offsetof(dsd_scan_settings, state_samplesPerSymbol),
        offsetof(dsd_scan_settings, state_symbolCenter), offsetof(dsd_scan_settings, state_dmr_stereo),
        offsetof(dsd_scan_settings, state_sps_hunt_idx),
    };
    if (!a || !b) {
        return 0;
    }
    return scan_settings_fields_equal(a, b, options, sizeof(options) / sizeof(options[0]))
           && strncmp(a->output_name, b->output_name, sizeof(a->output_name)) == 0
           && strncmp(a->group_in_file, b->group_in_file, sizeof(a->group_in_file)) == 0
           && (!include_timing || scan_settings_fields_equal(a, b, timing, sizeof(timing) / sizeof(timing[0])));
}

dsd_decode_mode_profile
dsd_scan_mode_profile(dsd_scan_mode mode) {
    return dsd_decode_mode_profile_for(
        (unsigned)mode < sizeof(mode_presets) / sizeof(mode_presets[0]) ? mode_presets[mode] : DSDCFG_MODE_AUTO);
}

dsd_scan_mode
dsd_scan_mode_active(const dsd_state* state) {
    const scan_scope* scope = scan_scope_get(state);
    return scope && !scope->suspended ? scope->mode : DSD_SCAN_MODE_INHERIT;
}

static void
scan_scope_apply_timing(const dsd_opts* opts, dsd_state* state, dsd_decode_mode_profile profile) {
    int input_rate = dsd_opts_current_input_timing_rate(opts);
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        const int live_rate = (int)dsd_rtl_stream_metrics_hook_output_rate_hz();
        if (live_rate > 0) {
            input_rate = live_rate;
        }
    }
    state->samplesPerSymbol = dsd_opts_compute_sps_rate(opts, profile.symbol_rate_hz, input_rate);
    state->symbolCenter = dsd_opts_symbol_center(state->samplesPerSymbol);
    state->sps_hunt_idx = (int)profile.sps_profile_index;
}

static void
scan_scope_apply_decoder(dsd_opts* opts, dsd_state* state, const scan_scope* scope) {
    dsd_scan_settings_restore(&scope->configured, opts, state);
    if (scope->mode == DSD_SCAN_MODE_INHERIT) {
        return;
    }
    opts->mod_p25p2_c4fm = 0;
    opts->mod_p25p2_profile_lock = 0;
    const int channels = opts->pulse_digi_out_channels;
    const int rate = opts->pulse_digi_rate_out;
    (void)dsd_apply_decode_mode_preset(mode_presets[scope->mode], DSD_DECODE_PRESET_PROFILE_CLI, opts, state);
    if (scope->mode == DSD_SCAN_MODE_P25) {
        opts->frame_dmr = 0;
        opts->frame_x2tdma = 0;
        DSD_SNPRINTF(opts->output_name, sizeof(opts->output_name), "%s", "P25");
    }
    opts->pulse_digi_out_channels = channels;
    opts->pulse_digi_rate_out = rate;
    if (scope->configured.mod_cli_lock) {
        opts->mod_c4fm = scope->configured.mod_c4fm;
        opts->mod_qpsk = scope->configured.mod_qpsk;
        opts->mod_gfsk = scope->configured.mod_gfsk;
        if (scope->mode == DSD_SCAN_MODE_P25) {
            opts->mod_p25p2_c4fm = scope->configured.mod_p25p2_c4fm;
            opts->mod_p25p2_profile_lock = scope->configured.mod_p25p2_profile_lock;
        }
        state->rf_mod = dsd_opts_modulation(opts);
    }
    if (scope->modulation != DSD_SCAN_MODULATION_INHERIT) {
        dsd_scan_mode_apply_modulation(opts, scope->mode, scope->modulation);
        state->rf_mod = dsd_opts_modulation(opts);
    }
    dsd_decode_mode_profile profile = dsd_scan_mode_profile(scope->mode);
    if (scope->mode == DSD_SCAN_MODE_P25 && opts->mod_p25p2_profile_lock
        && scope->configured.state_sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_6000_4) {
        profile = dsd_decode_mode_profile_for(DSDCFG_MODE_P25P2);
    }
    scan_scope_apply_timing(opts, state, profile);
}

static void
scan_options_apply(dsd_opts* opts, dsd_state* state, const dsd_scan_option_values* values) {
    const uint32_t present = values->present;
    if (present & DSD_SCAN_OPT_FORCE) {
        state->M = values->force;
    }
    if (present & DSD_SCAN_OPT_CRC) {
        opts->aggressive_framesync = (short)values->strict_crc;
        opts->dmr_crc_relaxed_default = (uint8_t)!values->strict_crc;
    }
    if (present & DSD_SCAN_OPT_VOICE) {
        opts->scan_voice_only = values->voice_only;
    }
    if (present & DSD_SCAN_OPT_QUALIFY) {
        opts->scan_voice_qualify_ms = values->qualify_ms;
    }
    if (present & DSD_SCAN_OPT_HOLD) {
        opts->scan_voice_hold_ms = values->hold_ms;
    }
    if (present & (DSD_SCAN_OPT_BP | DSD_SCAN_OPT_HYTERA)) {
        opts->dmr_mute_encL = values->mute_dmr;
        opts->dmr_mute_encR = values->mute_dmr;
    }
    if (present & DSD_SCAN_OPT_SCALAR) {
        opts->unmute_encrypted_p25 = values->unmute_p25;
    }
    if (present & DSD_SCAN_OPT_GROUP) {
        DSD_MEMCPY(opts->group_in_file, values->group_file, sizeof(opts->group_in_file));
    }
}

static void
scan_scope_apply(dsd_opts* opts, dsd_state* state, const scan_scope* scope) {
    scan_scope_apply_decoder(opts, state, scope);
    scan_options_apply(opts, state, &scope->options);
}

void
dsd_scan_mode_options(dsd_opts* opts, dsd_state* state, const dsd_scan_option_values* values) {
    scan_scope* scope = scan_scope_get(state);
    if (!scope || !opts) {
        return;
    }
    DSD_MEMSET(&scope->options, 0, sizeof(scope->options));
    if (values) {
        scope->options = *values;
    }
    if (!scope->suspended) {
        scan_settings_restore_row_opts(&scope->configured, opts);
        state->M = scope->configured.force_key;
        scan_options_apply(opts, state, &scope->options);
    }
}

int
dsd_scan_mode_begin(const dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return -1;
    }
    if (scan_scope_get(state)) {
        return 0;
    }
    scan_scope* scope = (scan_scope*)calloc(1, sizeof(*scope));
    if (!scope) {
        return -1;
    }
    dsd_scan_settings_capture(opts, state, &scope->configured);
    scope->configured_mode = dsd_infer_decode_mode_preset_exact(opts);
    (void)dsd_state_ext_set(state, DSD_STATE_EXT_RUNTIME_SCAN_MODE, scope, free);
    return 0;
}

int
dsd_scan_mode_enter(dsd_opts* opts, dsd_state* state, dsd_scan_mode mode) {
    if (!opts || !state || (unsigned)mode >= sizeof(mode_presets) / sizeof(mode_presets[0])) {
        return -1;
    }
    if (dsd_scan_mode_begin(opts, state) != 0) {
        return -1;
    }
    scan_scope* scope = scan_scope_get(state);
    if (!scope) {
        return -1;
    }
    DSD_MEMSET(&scope->options, 0, sizeof(scope->options));
    scope->modulation = 0;
    scope->mode = mode;
    scope->suspended = 0;
    scan_scope_apply(opts, state, scope);
    return 0;
}

dsdneoUserDecodeMode
dsd_scan_mode_configured_preset(const dsd_opts* opts, const dsd_state* state) {
    const dsdneoUserDecodeMode mode = dsd_scan_mode_configured_preset_exact(opts, state);
    return mode == DSDCFG_MODE_UNSET ? DSDCFG_MODE_AUTO : mode;
}

dsdneoUserDecodeMode
dsd_scan_mode_configured_preset_exact(const dsd_opts* opts, const dsd_state* state) {
    const scan_scope* scope = scan_scope_get(state);
    return scope && !scope->suspended ? scope->configured_mode : dsd_infer_decode_mode_preset_exact(opts);
}

void
dsd_scan_mode_leave(dsd_opts* opts, dsd_state* state) {
    scan_scope* scope = scan_scope_get(state);
    if (!scope || !opts) {
        return;
    }
    if (!scope->suspended) {
        dsd_scan_settings_restore(&scope->configured, opts, state);
    }
    (void)dsd_state_ext_set(state, DSD_STATE_EXT_RUNTIME_SCAN_MODE, NULL, NULL);
}

int
dsd_scan_mode_suspend(dsd_opts* opts, dsd_state* state) {
    scan_scope* scope = scan_scope_get(state);
    if (!scope || !opts || scope->suspended) {
        return 0;
    }
    dsd_scan_settings_capture(opts, state, &scope->effective);
    dsd_scan_settings_restore(&scope->configured, opts, state);
    scope->suspended = 1;
    return 1;
}

int
dsd_scan_mode_updating(const dsd_state* state) {
    const scan_scope* scope = scan_scope_get(state);
    return scope && scope->suspended;
}

int
dsd_scan_mode_resume(dsd_opts* opts, dsd_state* state) {
    scan_scope* scope = scan_scope_get(state);
    if (!scope || !opts || !state || !scope->suspended) {
        return 0;
    }
    dsd_scan_settings_capture(opts, state, &scope->configured);
    scope->configured_mode = dsd_infer_decode_mode_preset_exact(opts);
    scope->suspended = 0;
    scan_scope_apply(opts, state, scope);
    if (scope->mode == DSD_SCAN_MODE_P25) {
        /* Configuration updates do not move the receiver off its Phase 2 channel
         * or erase an unlocked modulation acquired on the current P25 row. */
        if (scope->effective.state_sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_6000_4) {
            scan_scope_apply_timing(opts, state, dsd_decode_mode_profile_for(DSDCFG_MODE_P25P2));
        }
        if (!opts->mod_cli_lock) {
            state->rf_mod = scope->effective.state_rf_mod;
        }
    }
    dsd_scan_settings effective;
    dsd_scan_settings_capture(opts, state, &effective);
    /* Monitoring is audio routing. Fold it into the restored effective values
     * without treating it as an acquisition change or replacing live timing. */
    scope->effective.monitor_input_audio = opts->monitor_input_audio;
    if (dsd_scan_settings_equal(&scope->effective, &effective, 0)) {
        dsd_scan_settings_restore(&scope->effective, opts, state);
        return 0;
    }
    return 1;
}

void
dsd_scan_mode_target_modulation(const dsd_state* state, dsd_scan_modulation modulation) {
    scan_scope* scope = scan_scope_get(state);
    if (scope && (unsigned)modulation <= DSD_SCAN_MODULATION_GFSK) {
        scope->modulation = modulation;
    }
}

void
dsd_scan_mode_apply_modulation(dsd_opts* opts, dsd_scan_mode mode, dsd_scan_modulation modulation) {
    if (!opts || modulation == DSD_SCAN_MODULATION_INHERIT || (unsigned)modulation > DSD_SCAN_MODULATION_GFSK) {
        return;
    }
    int mod;
    switch (modulation) {
        case DSD_SCAN_MODULATION_AUTO: mod = mode == DSD_SCAN_MODE_P25 ? 0 : 2; break;
        case DSD_SCAN_MODULATION_C4FM: mod = 0; break;
        case DSD_SCAN_MODULATION_CQPSK: mod = 1; break;
        case DSD_SCAN_MODULATION_GFSK: mod = 2; break;
        default: return;
    }
    opts->mod_cli_lock = modulation != DSD_SCAN_MODULATION_AUTO;
    opts->mod_p25p2_c4fm = 0;
    opts->mod_p25p2_profile_lock = 0;
    opts->mod_c4fm = mod == 0;
    opts->mod_qpsk = mod == 1;
    opts->mod_gfsk = mod == 2;
}

void
dsd_scan_mode_configured(const dsd_opts* opts, const dsd_state* state, dsd_scan_settings* out) {
    if (!opts || !state || !out) {
        return;
    }
    const scan_scope* scope = scan_scope_get(state);
    if (scope && !scope->suspended) {
        *out = scope->configured;
    } else {
        dsd_scan_settings_capture(opts, state, out);
    }
}

const dsd_scan_settings*
dsd_scan_mode_configured_view(const dsd_state* state) {
    const scan_scope* scope = scan_scope_get(state);
    return scope && !scope->suspended ? &scope->configured : NULL;
}

void
dsd_scan_mode_copy_snapshot(dsd_state* dst, const dsd_state* src) {
    if (!dst || dst == src) {
        return;
    }
    const scan_scope* source = scan_scope_get(src);
    if (!source) {
        (void)dsd_state_ext_set(dst, DSD_STATE_EXT_RUNTIME_SCAN_MODE, NULL, NULL);
        return;
    }
    scan_scope* copy = scan_scope_get(dst);
    if (!copy) {
        copy = (scan_scope*)calloc(1, sizeof(*copy));
        if (!copy) {
            return;
        }
        (void)dsd_state_ext_set(dst, DSD_STATE_EXT_RUNTIME_SCAN_MODE, copy, free);
    }
    copy->configured = source->configured;
    copy->options = source->options;
    copy->modulation = source->modulation;
    copy->mode = source->mode;
    copy->configured_mode = source->configured_mode;
    copy->suspended = source->suspended;
    /* The effective backup matters only between suspend and resume. A normal
     * published snapshot never consumes it; a later suspend captures its own. */
    if (source->suspended) {
        copy->effective = source->effective;
    }
}

dsd_decode_mode_profile
dsd_scan_mode_effective_profile(const dsd_opts* opts, const dsd_state* state) {
    const dsd_scan_mode mode = dsd_scan_mode_active(state);
    if (mode == DSD_SCAN_MODE_INHERIT && state) {
        /* AUTO/custom baselines may have been captured anywhere in the hunt.
         * The restored index, timing and frontend must describe the same profile. */
        switch (state->sps_hunt_idx) {
            case DSD_FRAME_SYNC_SPS_PROFILE_4800_4: return dsd_decode_mode_profile_for(DSDCFG_MODE_P25P1);
            case DSD_FRAME_SYNC_SPS_PROFILE_2400_4: return dsd_decode_mode_profile_for(DSDCFG_MODE_NXDN48);
            case DSD_FRAME_SYNC_SPS_PROFILE_9600_2: return dsd_decode_mode_profile_for(DSDCFG_MODE_EDACS_PV);
            case DSD_FRAME_SYNC_SPS_PROFILE_6000_4: return dsd_decode_mode_profile_for(DSDCFG_MODE_P25P2);
            case DSD_FRAME_SYNC_SPS_PROFILE_4800_2: return dsd_decode_mode_profile_for(DSDCFG_MODE_DSTAR);
            default: break;
        }
    }
    if (mode == DSD_SCAN_MODE_P25 && state->sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_6000_4) {
        return dsd_decode_mode_profile_for(DSDCFG_MODE_P25P2);
    }
    return mode != DSD_SCAN_MODE_INHERIT ? dsd_scan_mode_profile(mode)
                                         : dsd_decode_mode_profile_for(dsd_infer_decode_mode_preset(opts));
}

int
dsd_scan_mode_prepare(dsd_opts* opts, dsd_state* state, dsd_scan_mode mode, dsd_scan_settings* out) {
    if (!opts || !state || !out || (unsigned)mode >= sizeof(mode_presets) / sizeof(mode_presets[0])) {
        return -1;
    }
    if (dsd_scan_mode_begin(opts, state) != 0) {
        return -1;
    }
    dsd_scan_settings effective;
    dsd_scan_settings_capture(opts, state, &effective);
    scan_scope temporary;
    DSD_MEMSET(&temporary, 0, sizeof(temporary));
    dsd_scan_mode_configured(opts, state, &temporary.configured);
    temporary.mode = mode;
    if (mode == DSD_SCAN_MODE_INHERIT) {
        *out = temporary.configured;
    } else {
        scan_scope_apply(opts, state, &temporary);
        dsd_scan_settings_capture(opts, state, out);
        dsd_scan_settings_restore(&effective, opts, state);
    }
    return 0;
}
