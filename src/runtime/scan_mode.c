// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

#include <ctype.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* What the RTL demodulator holds while a row change is in progress. dsd_scan_mode_enter() leaves
 * the push to the dsd_scan_mode_options() call that completes the row, so one transition hands
 * the demod at most one level (a -60 dB row followed by another -60 dB row hands it none). */
typedef enum {
    SCAN_SQUELCH_SETTLED = 0, /* the demod holds what dsd_opts held before this entry point */
    SCAN_SQUELCH_ENTERED,     /* a row was entered; the demod still holds squelch_entered_from */
    SCAN_SQUELCH_UNKNOWN,     /* entered while suspended: a command may have pushed anything */
} scan_squelch_pending;

typedef struct {
    dsd_scan_settings configured;
    dsd_scan_settings effective;
    dsd_scan_modulation modulation;
    dsd_scan_mode mode;
    dsdneoUserDecodeMode configured_mode;
    int suspended;
    dsd_scan_option_values options;
    double squelch_entered_from;
    scan_squelch_pending squelch_pending;
} scan_scope;

static const char* const mode_names[] = {"", "p25", "dmr", "nxdn96", "nxdn48", "dpmr", "dstar", "ysf", "m17", "nfm"};
static const dsdneoUserDecodeMode mode_presets[] = {
    DSDCFG_MODE_AUTO, DSDCFG_MODE_TDMA,  DSDCFG_MODE_DMR, DSDCFG_MODE_NXDN96, DSDCFG_MODE_NXDN48,
    DSDCFG_MODE_DPMR, DSDCFG_MODE_DSTAR, DSDCFG_MODE_YSF, DSDCFG_MODE_M17,    DSDCFG_MODE_ANALOG};

_Static_assert(sizeof(mode_names) / sizeof(mode_names[0]) == (size_t)DSD_SCAN_MODE_LAST + 1U,
               "every scan class needs a name");
_Static_assert(sizeof(mode_presets) / sizeof(mode_presets[0]) == (size_t)DSD_SCAN_MODE_LAST + 1U,
               "every scan class needs a decode preset");

const char*
dsd_scan_mode_name(dsd_scan_mode mode) {
    return (unsigned)mode < sizeof(mode_names) / sizeof(mode_names[0]) ? mode_names[mode] : "";
}

int
dsd_scan_mode_is_analog(dsd_scan_mode mode) {
    return mode == DSD_SCAN_MODE_NFM;
}

/* Trim @p text into [*begin, *begin + *len). */
static void
scan_mode_trim(const char* text, const char** begin, size_t* len) {
    while (isspace((unsigned char)*text)) {
        text++;
    }
    size_t n = strlen(text);
    while (n && isspace((unsigned char)text[n - 1])) {
        n--;
    }
    *begin = text;
    *len = n;
}

/* Whether the trimmed @p text of @p len bytes is @p lower, ignoring ASCII case. */
static int
scan_mode_text_is(const char* text, size_t len, const char* lower) {
    if (strlen(lower) != len) {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        if (tolower((unsigned char)text[i]) != lower[i]) {
            return 0;
        }
    }
    return 1;
}

const char*
dsd_scan_mode_alias_hint(const char* text) {
    static const char* const analog_fm[] = {"fm", "analog", "wfm", "nbfm", "fm-conventional"};
    if (!text) {
        return NULL;
    }
    const char* begin = NULL;
    size_t len = 0;
    scan_mode_trim(text, &begin, &len);
    for (size_t i = 0; i < sizeof(analog_fm) / sizeof(analog_fm[0]); i++) {
        if (scan_mode_text_is(begin, len, analog_fm[i])) {
            return mode_names[DSD_SCAN_MODE_NFM];
        }
    }
    return NULL;
}

int
dsd_scan_mode_names_list(char* out, size_t out_size) {
    if (!out || out_size == 0U) {
        return -1;
    }
    out[0] = '\0';
    size_t used = 0;
    for (size_t i = DSD_SCAN_MODE_P25; i < sizeof(mode_names) / sizeof(mode_names[0]); i++) {
        const int n = DSD_SNPRINTF(out + used, out_size - used, "%s%s", used ? ", " : "", mode_names[i]);
        if (n < 0 || (size_t)n >= out_size - used) {
            out[0] = '\0';
            return -1;
        }
        used += (size_t)n;
    }
    return 0;
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
    const char* begin = NULL;
    size_t len = 0;
    scan_mode_trim(text, &begin, &len);
    for (size_t i = 0; i < sizeof(mode_names) / sizeof(mode_names[0]); i++) {
        if (scan_mode_text_is(begin, len, mode_names[i])) {
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
    out->rtl_squelch_level = opts->rtl_squelch_level;
    out->force_key = state->M;
    out->aggressive_framesync = opts->aggressive_framesync;
    out->dmr_crc_relaxed_default = opts->dmr_crc_relaxed_default;
    out->scan_voice_only = opts->scan_voice_only;
    out->scan_voice_qualify_ms = opts->scan_voice_qualify_ms;
    out->scan_voice_hold_ms = opts->scan_voice_hold_ms;
    out->scan_max_visit_ms = opts->scan_max_visit_ms;
    out->dmr_mute_encL = opts->dmr_mute_encL;
    out->dmr_mute_encR = opts->dmr_mute_encR;
    out->unmute_encrypted_p25 = opts->unmute_encrypted_p25;
    out->trunk_tune_data_calls = opts->trunk_tune_data_calls;
    out->trunk_tune_enc_calls = opts->trunk_tune_enc_calls;
    out->p25_prefer_candidates = opts->p25_prefer_candidates;
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
    out->analog_demod = opts->analog_demod;
    out->analog_nfm_bandwidth_hz = opts->analog_nfm_bandwidth_hz;
    out->analog_am_bandwidth_hz = opts->analog_am_bandwidth_hz;
    DSD_MEMCPY(out->output_name, opts->output_name, sizeof(out->output_name));
    out->state_rf_mod = state->rf_mod;
    out->state_samplesPerSymbol = state->samplesPerSymbol;
    out->state_symbolCenter = state->symbolCenter;
    out->state_dmr_stereo = state->dmr_stereo;
    out->state_sps_hunt_idx = state->sps_hunt_idx;
}

_Static_assert(sizeof(((dsd_scan_option_values*)0)->group_file) == sizeof(((dsd_opts*)0)->group_in_file),
               "row group paths are copied verbatim over dsd_opts::group_in_file");
_Static_assert(sizeof(((dsd_scan_settings*)0)->group_in_file) == sizeof(((dsd_opts*)0)->group_in_file),
               "scan settings snapshot dsd_opts::group_in_file verbatim");

static void
scan_settings_restore_row_opts(const dsd_scan_settings* saved, dsd_opts* opts) {
    opts->rtl_squelch_level = saved->rtl_squelch_level;
    opts->aggressive_framesync = saved->aggressive_framesync;
    opts->dmr_crc_relaxed_default = saved->dmr_crc_relaxed_default;
    opts->scan_voice_only = saved->scan_voice_only;
    opts->scan_voice_qualify_ms = saved->scan_voice_qualify_ms;
    opts->scan_voice_hold_ms = saved->scan_voice_hold_ms;
    opts->scan_max_visit_ms = saved->scan_max_visit_ms;
    opts->dmr_mute_encL = saved->dmr_mute_encL;
    opts->dmr_mute_encR = saved->dmr_mute_encR;
    opts->unmute_encrypted_p25 = saved->unmute_encrypted_p25;
    opts->trunk_tune_data_calls = saved->trunk_tune_data_calls;
    opts->trunk_tune_enc_calls = saved->trunk_tune_enc_calls;
    opts->p25_prefer_candidates = (uint8_t)saved->p25_prefer_candidates;
    DSD_MEMCPY(opts->group_in_file, saved->group_in_file, sizeof(opts->group_in_file));
}

/* Row-scoped nonsecret options are policy, not acquisition: they never restage a tune
 * or interrupt a parked row, so they travel beside the comparison, not through it. */
static void
scan_settings_copy_row_opts(dsd_scan_settings* dst, const dsd_scan_settings* src) {
    dst->rtl_squelch_level = src->rtl_squelch_level;
    dst->force_key = src->force_key;
    dst->aggressive_framesync = src->aggressive_framesync;
    dst->dmr_crc_relaxed_default = src->dmr_crc_relaxed_default;
    dst->scan_voice_only = src->scan_voice_only;
    dst->scan_voice_qualify_ms = src->scan_voice_qualify_ms;
    dst->scan_voice_hold_ms = src->scan_voice_hold_ms;
    dst->scan_max_visit_ms = src->scan_max_visit_ms;
    dst->dmr_mute_encL = src->dmr_mute_encL;
    dst->dmr_mute_encR = src->dmr_mute_encR;
    dst->unmute_encrypted_p25 = src->unmute_encrypted_p25;
    dst->trunk_tune_data_calls = src->trunk_tune_data_calls;
    dst->trunk_tune_enc_calls = src->trunk_tune_enc_calls;
    dst->p25_prefer_candidates = src->p25_prefer_candidates;
    DSD_MEMCPY(dst->group_in_file, src->group_in_file, sizeof(dst->group_in_file));
}

/* The analog channel widths are acquisition settings a row may override (DSD_SCAN_OPT_BANDWIDTH), so
 * they come back with the decoder settings and again before a row's options are installed. */
static void
scan_settings_restore_widths(const dsd_scan_settings* saved, dsd_opts* opts) {
    opts->analog_nfm_bandwidth_hz = saved->analog_nfm_bandwidth_hz;
    opts->analog_am_bandwidth_hz = saved->analog_am_bandwidth_hz;
}

static void
scan_settings_restore_opts(const dsd_scan_settings* saved, dsd_opts* opts) {
    scan_settings_restore_row_opts(saved, opts);
    scan_settings_restore_widths(saved, opts);
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
    opts->analog_demod = saved->analog_demod;
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
        offsetof(dsd_scan_settings, analog_demod),
        offsetof(dsd_scan_settings, analog_nfm_bandwidth_hz),
        offsetof(dsd_scan_settings, analog_am_bandwidth_hz),
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

static int
scan_configured_digital(const dsd_opts* opts, int configured_analog_only) {
    return (configured_analog_only == 1 && opts->m17encoder != 1) ? 0 : 1;
}

int
dsd_scan_mode_configured_digital(const dsd_opts* opts, const dsd_state* state) {
    if (!opts) {
        return 0;
    }
    const dsd_scan_settings* configured = dsd_scan_mode_configured_view(state);
    return scan_configured_digital(opts, configured ? configured->analog_only : opts->analog_only);
}

/* The rate a row with @p symbol_rate_hz (0: no symbol clock) is timed for; see dsd_scan_mode_symbol_timing_rate_hz(). */
static int
scan_timing_rate_hz(const dsd_opts* opts, int configured_digital, int symbol_rate_hz, int cqpsk) {
    const int input_rate = dsd_opts_current_input_timing_rate(opts);
    if (opts->audio_in_type != AUDIO_IN_RTL) {
        return input_rate;
    }
    if (symbol_rate_hz > 0 && configured_digital && dsd_rtl_stream_metrics_hook_analog_family_active()) {
        /* The tune switches the family, and the output rate with it, only once it lands on the demod thread. */
        const unsigned int landing_hz =
            dsd_rtl_stream_metrics_hook_output_rate_for_family(DSD_RX_FAMILY_DIGITAL, cqpsk ? 1 : 0, symbol_rate_hz);
        if (landing_hz > 0U) {
            return (int)landing_hz;
        }
    }
    const int live_rate = (int)dsd_rtl_stream_metrics_hook_output_rate_hz();
    return live_rate > 0 ? live_rate : input_rate;
}

int
dsd_scan_mode_symbol_timing_rate_hz(const dsd_opts* opts, const dsd_state* state, int symbol_rate_hz, int cqpsk) {
    if (!opts) {
        return 0;
    }
    return scan_timing_rate_hz(opts, dsd_scan_mode_configured_digital(opts, state), symbol_rate_hz, cqpsk);
}

/* Time the decoder for the row @p scope describes. The scope's own configured baseline says whether the configured
 * mode is digital: dsd_opts already holds the row's class here, and the configured view is gone while suspended. An
 * analog row has no symbol clock and keeps the live rate. */
static void
scan_scope_apply_timing(const dsd_opts* opts, dsd_state* state, const scan_scope* scope,
                        dsd_decode_mode_profile profile) {
    const int clock_hz = dsd_scan_mode_is_analog(scope->mode) ? 0 : profile.symbol_rate_hz;
    const int rate_hz = scan_timing_rate_hz(opts, scan_configured_digital(opts, scope->configured.analog_only),
                                            clock_hz, state->rf_mod == 1);
    state->samplesPerSymbol = dsd_opts_compute_sps_rate(opts, profile.symbol_rate_hz, rate_hz);
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
    scan_scope_apply_timing(opts, state, scope, profile);
}

typedef void (*scan_option_applier)(dsd_opts* opts, dsd_state* state, const dsd_scan_option_values* values);

static void
scan_option_apply_force(dsd_opts* opts, dsd_state* state, const dsd_scan_option_values* values) {
    (void)opts;
    state->M = values->force;
}

static void
scan_option_apply_crc(dsd_opts* opts, dsd_state* state, const dsd_scan_option_values* values) {
    (void)state;
    opts->aggressive_framesync = (short)values->strict_crc;
    opts->dmr_crc_relaxed_default = (uint8_t)!values->strict_crc;
}

static void
scan_option_apply_voice(dsd_opts* opts, dsd_state* state, const dsd_scan_option_values* values) {
    (void)state;
    opts->scan_voice_only = values->voice_only;
}

static void
scan_option_apply_qualify(dsd_opts* opts, dsd_state* state, const dsd_scan_option_values* values) {
    (void)state;
    opts->scan_voice_qualify_ms = values->qualify_ms;
}

static void
scan_option_apply_hold(dsd_opts* opts, dsd_state* state, const dsd_scan_option_values* values) {
    (void)state;
    opts->scan_voice_hold_ms = values->hold_ms;
}

static void
scan_option_apply_max_visit(dsd_opts* opts, dsd_state* state, const dsd_scan_option_values* values) {
    (void)state;
    opts->scan_max_visit_ms = values->max_visit_ms;
}

static void
scan_option_apply_mute_dmr(dsd_opts* opts, dsd_state* state, const dsd_scan_option_values* values) {
    (void)state;
    opts->dmr_mute_encL = values->mute_dmr;
    opts->dmr_mute_encR = values->mute_dmr;
}

static void
scan_option_apply_mute_p25(dsd_opts* opts, dsd_state* state, const dsd_scan_option_values* values) {
    (void)state;
    (void)values;
    /* Direct option text arms decryption; undecodable P25 audio stays muted. */
    opts->unmute_encrypted_p25 = 0;
}

static void
scan_option_apply_data(dsd_opts* opts, dsd_state* state, const dsd_scan_option_values* values) {
    (void)state;
    opts->trunk_tune_data_calls = values->tune_data_calls;
}

static void
scan_option_apply_p25_candidates(dsd_opts* opts, dsd_state* state, const dsd_scan_option_values* values) {
    (void)state;
    (void)values;
    opts->p25_prefer_candidates = 1;
}

static void
scan_option_apply_enc(dsd_opts* opts, dsd_state* state, const dsd_scan_option_values* values) {
    (void)state;
    opts->trunk_tune_enc_calls = values->tune_enc_calls;
}

static void
scan_option_apply_group(dsd_opts* opts, dsd_state* state, const dsd_scan_option_values* values) {
    (void)state;
    DSD_MEMCPY(opts->group_in_file, values->group_file, sizeof(opts->group_in_file));
}

static void
scan_option_apply_squelch(dsd_opts* opts, dsd_state* state, const dsd_scan_option_values* values) {
    (void)state;
    /* The rtl_sql conversion every other squelch entry point uses: 0 is off. */
    opts->rtl_squelch_level = dsd_squelch_level_from_sql((double)values->squelch_db);
}

static void
scan_option_apply_bandwidth(dsd_opts* opts, dsd_state* state, const dsd_scan_option_values* values) {
    (void)state;
    /* Only an NFM row parses a width (--nfm-bandwidth-hz), so it is the NFM demodulator's. */
    opts->analog_nfm_bandwidth_hz = values->channel_bw_hz;
}

/* One applier per row option that lands in dsd_opts/dsd_state. A new row option adds a row
 * here, never a branch: the lookup stays flat however many options the grammar grows. */
static const struct {
    uint32_t field;
    scan_option_applier apply;
} scan_option_appliers[] = {
    {DSD_SCAN_OPT_FORCE, scan_option_apply_force},
    {DSD_SCAN_OPT_CRC, scan_option_apply_crc},
    {DSD_SCAN_OPT_VOICE, scan_option_apply_voice},
    {DSD_SCAN_OPT_QUALIFY, scan_option_apply_qualify},
    {DSD_SCAN_OPT_HOLD, scan_option_apply_hold},
    {DSD_SCAN_OPT_MAX_VISIT, scan_option_apply_max_visit},
    {DSD_SCAN_OPT_MUTE_DMR, scan_option_apply_mute_dmr},
    {DSD_SCAN_OPT_MUTE_P25, scan_option_apply_mute_p25},
    {DSD_SCAN_OPT_DATA, scan_option_apply_data},
    {DSD_SCAN_OPT_P25_CANDIDATES, scan_option_apply_p25_candidates},
    {DSD_SCAN_OPT_ENC, scan_option_apply_enc},
    {DSD_SCAN_OPT_GROUP, scan_option_apply_group},
    {DSD_SCAN_OPT_SQUELCH, scan_option_apply_squelch},
    {DSD_SCAN_OPT_BANDWIDTH, scan_option_apply_bandwidth},
};

static void
scan_options_apply(dsd_opts* opts, dsd_state* state, const dsd_scan_option_values* values) {
    for (size_t i = 0; i < sizeof(scan_option_appliers) / sizeof(scan_option_appliers[0]); i++) {
        if (values->present & scan_option_appliers[i].field) {
            scan_option_appliers[i].apply(opts, state, values);
        }
    }
}

/* Squelch levels are mean powers spanning many decades (-100 dB is 1e-10), so "changed" is a
 * relative test; off (0.0) differs from every real threshold. */
static int
scan_squelch_changed(double before, double after) {
    return fabs(after - before) > 1e-9 * fmax(fabs(before), fabs(after));
}

/* Hand the RTL demodulator the squelch now in force. Only the scope's entry points call this:
 * prepare and scan_scope_apply() also run on temporary scopes and never reach the hardware. */
static void
scan_squelch_push(const dsd_opts* opts) {
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        (void)dsd_rtl_stream_metrics_hook_set_channel_squelch(opts->rtl_squelch_level);
    }
}

/* Push the level now in force when it differs from what the demod holds: `held` (dsd_opts before
 * this entry point), or the level from before a pending enter. A suspended scope already holds
 * the configured values in dsd_opts, while the demod was last told the row's threshold (suspend
 * does not push; resume does), so after an entry point that found the scope suspended the demod
 * level is unknown and the push is unconditional. */
static void
scan_squelch_settle(scan_scope* scope, const dsd_opts* opts, double held) {
    const scan_squelch_pending pending = scope->squelch_pending;
    scope->squelch_pending = SCAN_SQUELCH_SETTLED;
    if (pending == SCAN_SQUELCH_ENTERED) {
        held = scope->squelch_entered_from;
    }
    if (pending == SCAN_SQUELCH_UNKNOWN || scan_squelch_changed(held, opts->rtl_squelch_level)) {
        scan_squelch_push(opts);
    }
}

static void
scan_scope_apply(dsd_opts* opts, dsd_state* state, const scan_scope* scope) {
    scan_scope_apply_decoder(opts, state, scope);
    scan_options_apply(opts, state, &scope->options);
}

int
dsd_scan_mode_options(dsd_opts* opts, dsd_state* state, const dsd_scan_option_values* values) {
    scan_scope* scope = scan_scope_get(state);
    if (!scope || !opts) {
        return -1;
    }
    DSD_MEMSET(&scope->options, 0, sizeof(scope->options));
    if (values) {
        scope->options = *values;
    }
    if (!scope->suspended) {
        const double squelch_before = opts->rtl_squelch_level;
        scan_settings_restore_row_opts(&scope->configured, opts);
        scan_settings_restore_widths(&scope->configured, opts);
        state->M = scope->configured.force_key;
        scan_options_apply(opts, state, &scope->options);
        scan_squelch_settle(scope, opts, squelch_before);
    }
    return 0;
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
    const double squelch_before = opts->rtl_squelch_level;
    const int was_suspended = scope->suspended;
    DSD_MEMSET(&scope->options, 0, sizeof(scope->options));
    scope->modulation = 0;
    scope->mode = mode;
    scope->suspended = 0;
    scan_scope_apply(opts, state, scope);
    /* No push here: the row's own options follow, and dsd_scan_mode_options() pushes the net
     * change once. A transition already pending keeps its origin, since the demod has heard
     * nothing since. */
    if (was_suspended) {
        scope->squelch_pending = SCAN_SQUELCH_UNKNOWN;
    } else if (scope->squelch_pending == SCAN_SQUELCH_SETTLED) {
        scope->squelch_entered_from = squelch_before;
        scope->squelch_pending = SCAN_SQUELCH_ENTERED;
    }
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
    const double squelch_before = opts->rtl_squelch_level;
    if (scope->suspended) {
        scope->squelch_pending = SCAN_SQUELCH_UNKNOWN;
    } else {
        dsd_scan_settings_restore(&scope->configured, opts, state);
    }
    scan_squelch_settle(scope, opts, squelch_before);
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
            scan_scope_apply_timing(opts, state, scope, dsd_decode_mode_profile_for(DSDCFG_MODE_P25P2));
        }
        if (!opts->mod_cli_lock) {
            state->rf_mod = scope->effective.state_rf_mod;
        }
    }
    dsd_scan_settings effective;
    dsd_scan_settings_capture(opts, state, &effective);
    /* Monitoring is audio routing, and the row-scoped options are policy (forcing, CRC,
     * mutes, voice gate, group file). Fold both into the restored effective values
     * without treating them as an acquisition change or replacing live timing. */
    scope->effective.monitor_input_audio = opts->monitor_input_audio;
    scan_settings_copy_row_opts(&scope->effective, &effective);
    const int unchanged = dsd_scan_settings_equal(&scope->effective, &effective, 0);
    if (unchanged) {
        dsd_scan_settings_restore(&scope->effective, opts, state);
    }
    /* Unconditional: the command that ran while suspended may have pushed the configured
     * default straight to the demod (CONFIG_APPLY does), so a comparison with the pre-suspend
     * value would leave the row's threshold behind. */
    scope->squelch_pending = SCAN_SQUELCH_SETTLED;
    scan_squelch_push(opts);
    return unchanged ? 0 : 1;
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

uint32_t
dsd_scan_mode_option_fields(const dsd_state* state) {
    const scan_scope* scope = scan_scope_get(state);
    return scope ? scope->options.present : 0;
}

const dsd_scan_option_values*
dsd_scan_mode_row_options(const dsd_state* state) {
    const scan_scope* scope = scan_scope_get(state);
    return scope ? &scope->options : NULL;
}

int
dsd_scan_mode_set_configured_squelch(dsd_opts* opts, const dsd_state* state, double level) {
    if (!opts) {
        return -1;
    }
    scan_scope* scope = state ? scan_scope_get(state) : NULL;
    /* Suspended or absent, dsd_opts holds the configured values and resume recaptures them. */
    if (scope && !scope->suspended) {
        scope->configured.rtl_squelch_level = level;
        if (scope->options.present & DSD_SCAN_OPT_SQUELCH) {
            return 0;
        }
        /* The caller hands this level to the demod, so a row change still waiting for its
         * options judges against it. */
        if (scope->squelch_pending == SCAN_SQUELCH_ENTERED) {
            scope->squelch_entered_from = level;
        }
    }
    opts->rtl_squelch_level = level;
    return 1;
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
dsd_scan_mode_prepare(dsd_opts* opts, dsd_state* state, dsd_scan_mode mode, const dsd_scan_option_values* row,
                      dsd_scan_settings* out) {
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
    if (row) {
        temporary.options = *row;
    }
    if (mode == DSD_SCAN_MODE_INHERIT) {
        *out = temporary.configured;
    } else {
        scan_scope_apply(opts, state, &temporary);
        dsd_scan_settings_capture(opts, state, out);
        dsd_scan_settings_restore(&effective, opts, state);
    }
    return 0;
}
