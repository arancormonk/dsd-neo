// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The scan quiet check (issue #834): see scan_quiet.h. */

#include "scan_quiet.h"

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/freq_parse.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "scan_owner.h"
#ifdef USE_RADIO
#include <dsd-neo/engine/scan_voice_gate.h>
#include <dsd-neo/io/rtl_demod_config.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/io/signal_measure.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/signal_search.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#endif
#include "dsd-neo/runtime/analog_channel.h"
#include "scan_landing.h"

/* Width classes (the values of DSD_SIGNAL_QUIET_CLASS_*, which signal_measure.h, a radio header, names). */
enum { QUIET_CLASS_12K5 = 1, QUIET_CLASS_6K25 = 2, QUIET_CLASS_ANALOG = 4 };

#ifdef DSD_NEO_TEST_HOOKS
uint64_t (*scan_quiet_test_realtime_ms)(void) = NULL;
#endif

static const dsd_engine_scan_owner*
quiet_owner_const(const dsd_state* state) {
    return state ? (const dsd_engine_scan_owner*)dsd_state_ext_get_const(state, DSD_STATE_EXT_ENGINE_TRUNK_SCAN) : NULL;
}

void
scan_quiet_revisions_read(const dsd_opts* opts, const dsd_state* state, scan_quiet_revisions* out) {
    if (!out) {
        return;
    }
    DSD_MEMSET(out, 0, sizeof *out);
    if (!opts || !state) {
        return;
    }
    const dsd_engine_scan_owner* owner = quiet_owner_const(state);
    out->chan_map_seq = state->trunk_chan_map_seq;
    out->lcn_list_rev = state->lcn_list_rev;
    out->lcn_avoid_rev = state->lcn_avoid_rev;
    out->trunk_scan_avoid_rev = state->trunk_scan_avoid_rev;
    out->quiet_inv_seq = owner ? owner->quiet_inv_seq : 0U;
    out->lcn_scan_session = state->lcn_scan_session;
    out->trunk_scan_lists_seq = state->trunk_scan_lists_seq;
    out->trunk_edit_seq = dsd_engine_trunk_scan_quiet_edit_seq(state);
    out->y_edits_epoch = dsd_engine_channel_scan_quiet_edits_epoch(state);
    out->pcm_input_generation = opts->pcm_input_generation;
#ifdef USE_RADIO
    /* A receive-profile or width request still in flight changes what the channel is received as; one that repeats the
       profile the stream runs (the hunt re-asking for its own) changes nothing and moves no change number (issue #834
       R5). */
    out->receive_change_seq = rtl_stream_receive_change_seq();
#endif
    out->nfm_width_hz = opts->analog_nfm_bandwidth_hz;
    out->am_width_hz = opts->analog_am_bandwidth_hz;
}

uint8_t
scan_quiet_mode_classes(dsd_scan_mode mode) {
    switch (mode) {
        case DSD_SCAN_MODE_P25:
        case DSD_SCAN_MODE_DMR:
        case DSD_SCAN_MODE_NXDN96:
        case DSD_SCAN_MODE_YSF:
        case DSD_SCAN_MODE_M17: return QUIET_CLASS_12K5;
        case DSD_SCAN_MODE_NXDN48:
        case DSD_SCAN_MODE_DPMR:
        case DSD_SCAN_MODE_DSTAR: return QUIET_CLASS_6K25;
        case DSD_SCAN_MODE_NFM:
        case DSD_SCAN_MODE_AM: return QUIET_CLASS_ANALOG;
        /* No class: the check has no TETRA calibration (a decided limit, issue #890), so a tetra-dmo row or target is
           never checked and says why (DSD_SCAN_QUIET_REASON_UNSUPPORTED_MODE). */
        case DSD_SCAN_MODE_TETRA_DMO:
        case DSD_SCAN_MODE_INHERIT: return 0U;
    }
    return 0U;
}

uint8_t
scan_quiet_mode_dmr(const dsd_opts* opts, dsd_scan_mode mode) {
    if (mode == DSD_SCAN_MODE_DMR) {
        return 1U;
    }
    return (uint8_t)(mode == DSD_SCAN_MODE_INHERIT && opts && opts->frame_dmr ? 1U : 0U);
}

uint8_t
scan_quiet_visit_dmr(const dsd_opts* opts, dsd_scan_mode mode) {
    const uint32_t setting = scan_quiet_setting_windows(opts);
    return (uint8_t)(setting != 0U && setting < SCAN_QUIET_DMR_MIN_WINDOWS ? scan_quiet_mode_dmr(opts, mode) : 0U);
}

uint32_t
scan_quiet_setting_windows(const dsd_opts* opts) {
    if (!opts || opts->scan_skip_silent_ms <= 0) {
        return 0U;
    }
    return (uint32_t)(opts->scan_skip_silent_ms + 9) / 10U;
}

uint32_t
scan_quiet_request_windows(uint32_t setting_windows, int dmr) {
    if (setting_windows == 0U) {
        return 0U;
    }
    return dmr && setting_windows < SCAN_QUIET_DMR_MIN_WINDOWS ? (uint32_t)SCAN_QUIET_DMR_MIN_WINDOWS : setting_windows;
}

uint32_t
scan_quiet_request_deadline_ms(uint32_t windows) {
    const uint32_t calibrated = 9U; /* DSD_SIGNAL_QUIET_WINDOWS, which a radio header names */
    const uint32_t extra = windows > calibrated ? (windows - calibrated) * SCAN_QUIET_DEADLINE_PER_WINDOW_MS : 0U;
    return SCAN_QUIET_REQUEST_DEADLINE_MS + extra;
}

/* Whether the decoder set enables a mode of the 12.5 kHz class, and of the 6.25 kHz class. */
static int
quiet_decoder_wide(const dsd_opts* opts) {
    return opts->frame_p25p1 || opts->frame_p25p2 || opts->frame_dmr || opts->frame_nxdn96 || opts->frame_ysf
           || opts->frame_m17;
}

static int
quiet_decoder_narrow(const dsd_opts* opts) {
    return opts->frame_nxdn48 || opts->frame_dpmr || opts->frame_dstar;
}

uint32_t
scan_quiet_analog_width_hz(int kind, int explicit_width_hz) {
#ifdef USE_RADIO
    /* As the stream requests it: an explicit width as typed, the AM default (a definite filtered channel) when unset,
       and none for an unset NFM width, which keeps the historical enable rule and WIDE fallback. */
    const int width_hz = rtl_demod_analog_requested_width_hz(kind, explicit_width_hz);
#else
    (void)kind;
    const int width_hz = explicit_width_hz; /* no stream: no default to resolve, and no check is ever made */
#endif
    return width_hz > 0 ? (uint32_t)width_hz : 0U;
}

void
scan_quiet_analog_facts(const dsd_opts* opts, const dsd_state* state, int kind, const dsd_scan_option_values* values,
                        scan_quiet_eligibility* e) {
    if (!e) {
        return;
    }
    const int explicit_hz = (values && (values->present & DSD_SCAN_OPT_BANDWIDTH))
                                ? values->channel_bw_hz
                                : dsd_scan_mode_configured_analog_width(opts, state, kind);
    e->analog_kind = kind;
    e->analog_width_hz = scan_quiet_analog_width_hz(kind, explicit_hz);
}

/* The analog class's kind and the width the stream runs it at (scan_quiet_analog_width_hz() of the configured width),
   into @p analog_kind and @p analog_width_hz when given; 0 when that width is none, so the set gets no check. */
static uint8_t
quiet_decoder_analog(const dsd_opts* opts, int* analog_kind, uint32_t* analog_width_hz) {
    const uint32_t width_hz = scan_quiet_analog_width_hz(opts->analog_demod, dsd_opts_analog_width_hz(opts));
    if (width_hz == 0U) {
        return 0U;
    }
    if (analog_kind) {
        *analog_kind = opts->analog_demod;
    }
    if (analog_width_hz) {
        *analog_width_hz = width_hz;
    }
    return QUIET_CLASS_ANALOG;
}

int
scan_quiet_decoder_set_unchecked(const dsd_opts* opts) {
    return opts && (opts->frame_x2tdma || opts->frame_provoice || opts->frame_tetra);
}

uint8_t
scan_quiet_decoder_classes(const dsd_opts* opts, int* analog_kind, uint32_t* analog_width_hz) {
    if (analog_kind) {
        *analog_kind = 0;
    }
    if (analog_width_hz) {
        *analog_width_hz = 0U;
    }
    if (!opts || opts->m17encoder == 1) {
        return 0U;
    }
    if (dsd_opts_is_analog_family(opts)) {
        return quiet_decoder_analog(opts, analog_kind, analog_width_hz);
    }
    /* ProVoice and X2-TDMA have no sourced width and time structure, nor TETRA a class: a set holding one gets no
       check. */
    if (scan_quiet_decoder_set_unchecked(opts)) {
        return 0U;
    }
    const uint8_t digital = (uint8_t)((quiet_decoder_wide(opts) ? QUIET_CLASS_12K5 : 0U)
                                      | (quiet_decoder_narrow(opts) ? QUIET_CLASS_6K25 : 0U));
    return scan_quiet_monitor_classes(opts, digital, analog_kind, analog_width_hz);
}

uint8_t
scan_quiet_monitor_classes(const dsd_opts* opts, uint8_t digital, int* analog_kind, uint32_t* analog_width_hz) {
    if (!opts || opts->monitor_input_audio != 1 || digital == 0U) {
        return digital;
    }
    /* The analog monitor runs beside the digital decoders (-8): a mixed set, judged in the analog class too; a
       monitor the stream runs at no definite width (an unset NFM width) gets no check. */
    const uint8_t analog = quiet_decoder_analog(opts, analog_kind, analog_width_hz);
    return analog ? (uint8_t)(digital | analog) : 0U;
}

int
scan_quiet_format_skip(char* out, size_t out_size, const char* name, int row, uint32_t hz, float power_dbfs,
                       float floor_dbfs, float ratio_db) {
    char mhz[24];
    (void)dsd_freq_hz_mhz_text(hz, mhz, sizeof mhz);
    char row_name[24];
    if (!name || name[0] == '\0') {
        DSD_SNPRINTF(row_name, sizeof row_name, "row %d", row + 1);
        name = row_name;
    }
    return DSD_SNPRINTF(out, out_size, "NOTICE: scan: skipped quiet %s %s MHz: %.1f dBFS, floor %.1f dBFS (%.1f dB)\n",
                        name, mhz, (double)power_dbfs, (double)floor_dbfs, (double)ratio_db);
}

uint8_t
scan_quiet_facts_reason(const dsd_opts* opts, const scan_quiet_eligibility* e, dsd_scan_mode mode) {
    if (!e || (e->classes != 0U && !((e->classes & QUIET_CLASS_ANALOG) && e->analog_width_hz == 0U))) {
        return (uint8_t)DSD_SCAN_QUIET_REASON_NONE;
    }
    /* A class the check has no calibration for: nothing set on the row would give it one, so neither the width's text
       nor the decoders' applies (issue #890). */
    if (mode == DSD_SCAN_MODE_TETRA_DMO) {
        return (uint8_t)DSD_SCAN_QUIET_REASON_UNSUPPORTED_MODE;
    }
    const int decoder_set = mode == DSD_SCAN_MODE_INHERIT;
    /* No class from the decoder set itself: a mode with no known width, the encoder, or no mode of a class. An analog
       session, or a digital set whose -8 monitor has no width, is the width's. */
    if (decoder_set && opts && !dsd_opts_is_analog_family(opts)
        && (opts->m17encoder == 1 || scan_quiet_decoder_set_unchecked(opts)
            || !(quiet_decoder_wide(opts) || quiet_decoder_narrow(opts)))) {
        return (uint8_t)DSD_SCAN_QUIET_REASON_MODES;
    }
    return (uint8_t)DSD_SCAN_QUIET_REASON_NO_WIDTH;
}

int
scan_quiet_stay_shows_status(uint8_t stay_reason) {
    return stay_reason == (uint8_t)DSD_SCAN_STAY_IDLE_DWELL || stay_reason == (uint8_t)DSD_SCAN_STAY_HANGTIME;
}

/* The analog widths the WARN line names as checked (issue #834 R6): the calibration's admitted widths
   (signal_measure.cpp kQuietNfmWidthsHz, kQuietAmWidthsHz), which ENGINE_NO_CARRIER_RESET holds them to. */
static const uint32_t k_quiet_warn_nfm_hz[] = {8000U, 10000U, 11250U, 12000U, 12500U, 15000U, 16000U, 20000U, 25000U};
static const uint32_t k_quiet_warn_am_hz[] = {5000U, 6000U, 8000U, 8333U, 10000U, 15000U, 20000U};

const uint32_t*
scan_quiet_warn_widths(int kind, size_t* count) {
    const int am = kind == DSD_ANALOG_DEMOD_AM;
    if (count) {
        *count = am ? sizeof k_quiet_warn_am_hz / sizeof k_quiet_warn_am_hz[0]
                    : sizeof k_quiet_warn_nfm_hz / sizeof k_quiet_warn_nfm_hz[0];
    }
    return am ? k_quiet_warn_am_hz : k_quiet_warn_nfm_hz;
}

/* @p value / @p scale with up to @p digits decimals and no trailing zeros ("15", "11.25", "8.333"), locale-free. */
static void
quiet_scaled_text(uint32_t value, uint32_t scale, int digits, char* out, size_t out_size) {
    char frac[16];
    DSD_SNPRINTF(frac, sizeof frac, "%0*u", digits, (unsigned)(value % scale));
    size_t len = strlen(frac);
    while (len > 0U && frac[len - 1U] == '0') {
        frac[--len] = '\0';
    }
    if (len == 0U) {
        DSD_SNPRINTF(out, out_size, "%u", (unsigned)(value / scale));
    } else {
        DSD_SNPRINTF(out, out_size, "%u.%s", (unsigned)(value / scale), frac);
    }
}

/* The admitted widths of @p kind as the WARN line lists them: "8000, 10000, ... or 25000". */
static void
quiet_widths_text(int kind, char* out, size_t out_size) {
    size_t count = 0U;
    const uint32_t* widths = scan_quiet_warn_widths(kind, &count);
    size_t used = 0U;
    out[0] = '\0';
    for (size_t i = 0U; i < count && used < out_size; i++) {
        const char* sep = i == 0U ? "" : (i + 1U == count ? " or " : ", ");
        const int n = DSD_SNPRINTF(out + used, out_size - used, "%s%u", sep, (unsigned)widths[i]);
        used = n > 0 ? used + (size_t)n : out_size;
    }
}

/* The receiver's WARN lines: what has no check, what to do, and that the scan keeps its timing. */
static int
quiet_format_receiver_warn(char* out, size_t out_size, int detail, uint32_t rate_hz) {
    switch (detail) {
        case SCAN_QUIET_WARN_AUDIO:
            return DSD_SNPRINTF(out, out_size,
                                "WARNING: scan: silent channel skip cannot check channels on audio input; it needs a "
                                "radio input (rtl, rtltcp or soapy). Normal scan timing applies.\n");
        case SCAN_QUIET_WARN_REPLAY:
            return DSD_SNPRINTF(out, out_size,
                                "WARNING: scan: silent channel skip cannot check channels in an I/Q replay; it needs a "
                                "live radio input. Normal scan timing applies.\n");
        case SCAN_QUIET_WARN_STREAM:
            return DSD_SNPRINTF(out, out_size,
                                "WARNING: scan: silent channel skip cannot check channels on an I/Q stream; it needs a "
                                "radio input DSD-neo tunes. Normal scan timing applies.\n");
        case SCAN_QUIET_WARN_NO_RADIO:
            return DSD_SNPRINTF(out, out_size,
                                "WARNING: scan: silent channel skip cannot check channels: this build has no radio "
                                "support. Normal scan timing applies.\n");
        case SCAN_QUIET_WARN_AIRSPY:
            return DSD_SNPRINTF(out, out_size,
                                "WARNING: scan: silent channel skip cannot check channels on a native Airspy input; "
                                "open the Airspy through SoapySDR (-i soapy:driver=airspy) to have them checked. "
                                "Normal scan timing applies.\n");
        case SCAN_QUIET_WARN_CENTRED:
            return DSD_SNPRINTF(out, out_size,
                                "WARNING: scan: silent channel skip cannot check channels while offset tuning is on or "
                                "DSD_NEO_DISABLE_FS4_SHIFT is set; set DSD_NEO_RTL_OFFSET_TUNING=0, or unset "
                                "DSD_NEO_DISABLE_FS4_SHIFT, to have them checked. Normal scan timing applies.\n");
        default: break;
    }
    char rate[24];
    quiet_scaled_text(rate_hz, 1000000U, 6, rate, sizeof rate);
    return DSD_SNPRINTF(out, out_size,
                        "WARNING: scan: silent channel skip cannot check channels at %s Msps, a sample rate it was "
                        "not calibrated for; docs/trunk-scan.md lists the rates it checks. Normal scan timing "
                        "applies.\n",
                        rate);
}

uint8_t
scan_quiet_channel_detail(const dsd_opts* opts, uint8_t reason, uint8_t classes) {
    if (!opts) {
        return 0U;
    }
    if (reason == (uint8_t)DSD_SCAN_QUIET_REASON_MODES) {
        if (opts->m17encoder == 1) {
            return (uint8_t)SCAN_QUIET_WARN_ENCODER;
        }
        return (uint8_t)((opts->frame_provoice ? SCAN_QUIET_WARN_PROVOICE : 0)
                         | (opts->frame_x2tdma ? SCAN_QUIET_WARN_X2TDMA : 0)
                         | (opts->frame_tetra ? SCAN_QUIET_WARN_TETRA : 0));
    }
    /* No class and no analog session: the width missing is the -8 monitor's, beside a digital decoder. */
    if (reason == (uint8_t)DSD_SCAN_QUIET_REASON_NO_WIDTH && classes == 0U && !dsd_opts_is_analog_family(opts)) {
        return (uint8_t)SCAN_QUIET_WARN_MONITOR;
    }
    return 0U;
}

/* The modes of @p detail (SCAN_QUIET_WARN_PROVOICE, _X2TDMA, _TETRA) as a line names them: "ProVoice", "ProVoice and
   TETRA", "ProVoice, X2-TDMA and TETRA". */
static void
quiet_modes_text(uint8_t detail, char* out, size_t out_size) {
    const char* names[3];
    size_t count = 0U;
    if (detail & SCAN_QUIET_WARN_PROVOICE) {
        names[count++] = "ProVoice";
    }
    if (detail & SCAN_QUIET_WARN_X2TDMA) {
        names[count++] = "X2-TDMA";
    }
    if (detail & SCAN_QUIET_WARN_TETRA) {
        names[count++] = "TETRA";
    }
    out[0] = '\0';
    size_t used = 0U;
    for (size_t i = 0U; i < count && used < out_size; i++) {
        const char* sep = i == 0U ? "" : (i + 1U == count ? " and " : ", ");
        const int n = DSD_SNPRINTF(out + used, out_size - used, "%s%s", sep, names[i]);
        used = n > 0 ? used + (size_t)n : out_size;
    }
}

/* The MODES lines: the decoders in use, as they are, give the row no class. */
static int
quiet_format_modes_warn(char* out, size_t out_size, uint8_t detail, const char* who, const char* mhz) {
    if (detail & SCAN_QUIET_WARN_ENCODER) {
        return DSD_SNPRINTF(out, out_size,
                            "WARNING: scan: silent channel skip cannot check %s %s MHz while the M17 encoder runs; "
                            "turn the encoder off to have it checked. Normal scan timing applies.\n",
                            who, mhz);
    }
    if ((detail & (SCAN_QUIET_WARN_PROVOICE | SCAN_QUIET_WARN_X2TDMA | SCAN_QUIET_WARN_TETRA)) == 0U) {
        return DSD_SNPRINTF(out, out_size,
                            "WARNING: scan: silent channel skip cannot check %s %s MHz: none of the decoders in use "
                            "has a known channel width; give the row a mode (the mode column), or turn on a decoder "
                            "it checks (P25, DMR, NXDN, YSF, M17, dPMR or D-STAR), to have it checked. Normal scan "
                            "timing applies.\n",
                            who, mhz);
    }
    char modes[48];
    quiet_modes_text(detail, modes, sizeof modes);
    const int several = (detail & (detail - 1U) & 7U) != 0U;
    return DSD_SNPRINTF(out, out_size,
                        "WARNING: scan: silent channel skip cannot check %s %s MHz: %s, among the decoders in use, %s "
                        "no known channel width; give the row a mode (the mode column), or turn %s off, to have it "
                        "checked. Normal scan timing applies.\n",
                        who, mhz, modes, several ? "have" : "has", several ? "them" : "it");
}

/* The channel's WARN lines, for @p who at @p mhz; @p detail is scan_quiet_channel_detail()'s. */
static int
quiet_format_channel_warn(char* out, size_t out_size, uint8_t reason, uint8_t detail, const char* who, const char* mhz,
                          int kind, uint32_t width_hz) {
    if (reason == (uint8_t)DSD_SCAN_QUIET_REASON_MODES) {
        return quiet_format_modes_warn(out, out_size, detail, who, mhz);
    }
    if (reason == (uint8_t)DSD_SCAN_QUIET_REASON_UNSUPPORTED_MODE) {
        return DSD_SNPRINTF(out, out_size,
                            "WARNING: scan: silent channel skip cannot check %s %s MHz: it has no check for TETRA DMO "
                            "channels. Normal scan timing applies.\n",
                            who, mhz);
    }
    if (reason == (uint8_t)DSD_SCAN_QUIET_REASON_NO_WIDTH && (detail & SCAN_QUIET_WARN_MONITOR)) {
        return DSD_SNPRINTF(out, out_size,
                            "WARNING: scan: silent channel skip cannot check %s %s MHz: the -8 analog monitor beside "
                            "its decoder has no NFM width set; set --nfm-bandwidth-hz for the session, or turn -8 off, "
                            "to have it checked. Normal scan timing applies.\n",
                            who, mhz);
    }
    if (reason == (uint8_t)DSD_SCAN_QUIET_REASON_NO_WIDTH) {
        return DSD_SNPRINTF(out, out_size,
                            "WARNING: scan: silent channel skip cannot check %s %s MHz: it has no NFM width set; set "
                            "--nfm-bandwidth-hz for it, or for the session, to have it checked. Normal scan timing "
                            "applies.\n",
                            who, mhz);
    }
    char width[24];
    quiet_scaled_text(width_hz, 1000U, 3, width, sizeof width);
    char widths[96];
    quiet_widths_text(kind, widths, sizeof widths);
    const int am = kind == DSD_ANALOG_DEMOD_AM;
    return DSD_SNPRINTF(out, out_size,
                        "WARNING: scan: silent channel skip cannot check %s %s MHz at %s %s kHz, a width it was not "
                        "calibrated for; set --%s-bandwidth-hz to %s to have it checked. Normal scan timing "
                        "applies.\n",
                        who, mhz, am ? "AM" : "NFM", width, am ? "am" : "nfm", widths);
}

int
scan_quiet_format_warn(char* out, size_t out_size, uint8_t reason, int detail, uint32_t rate_hz, const char* name,
                       int row, uint32_t hz, int kind, uint32_t width_hz) {
    if (!out || out_size == 0U) {
        return -1;
    }
    if (reason == (uint8_t)DSD_SCAN_QUIET_REASON_INPUT || reason == (uint8_t)DSD_SCAN_QUIET_REASON_TUNER
        || reason == (uint8_t)DSD_SCAN_QUIET_REASON_RATE) {
        return quiet_format_receiver_warn(out, out_size, reason == (uint8_t)DSD_SCAN_QUIET_REASON_RATE ? -1 : detail,
                                          rate_hz);
    }
    char mhz[24];
    (void)dsd_freq_hz_mhz_text(hz, mhz, sizeof mhz);
    char row_name[24];
    if (!name || name[0] == '\0') {
        DSD_SNPRINTF(row_name, sizeof row_name, "row %d", row + 1);
        name = row_name;
    }
    return quiet_format_channel_warn(out, out_size, reason, (uint8_t)detail, name, mhz, kind, width_hz);
}

/* The receiver's WARN key (issue #834 R6): the reason, its detail, the input kind, the device spec and the capture
   rate. */
typedef struct {
    uint8_t reason;
    uint8_t detail;
    int input;
    uint32_t device;
    uint32_t rate_hz;
} quiet_receiver_key;

/* FNV-1a of the input spec: the device the key names. Read only while a receiver reason is in force. */
static uint32_t
quiet_device_hash(const dsd_opts* opts) {
    uint32_t h = 2166136261U;
    for (size_t i = 0U; i < sizeof opts->audio_in_dev && opts->audio_in_dev[i] != '\0'; i++) {
        h = (h ^ (uint8_t)opts->audio_in_dev[i]) * 16777619U;
    }
    return h;
}

/* Log the receiver's WARN line for @p key once: only when it differs from the last one logged. */
static void
quiet_warn_receiver(dsd_state* state, const quiet_receiver_key* key) {
    dsd_engine_scan_owner* owner = dsd_engine_scan_owner_ensure(state);
    if (!owner
        || (owner->quiet_warned_reason == key->reason && owner->quiet_warned_detail == key->detail
            && owner->quiet_warned_input == key->input && owner->quiet_warned_device == key->device
            && owner->quiet_warned_rate_hz == key->rate_hz)) {
        return;
    }
    owner->quiet_warned_reason = key->reason;
    owner->quiet_warned_detail = key->detail;
    owner->quiet_warned_input = key->input;
    owner->quiet_warned_device = key->device;
    owner->quiet_warned_rate_hz = key->rate_hz;
    char line[384];
    (void)scan_quiet_format_warn(line, sizeof line, key->reason, key->detail, key->rate_hz, NULL, 0, 0U, 0, 0U);
    LOG_WARN("%s", line);
}

/* Forget the receiver's last WARN line (the setting turned): the next one is logged again. */
static void
quiet_warn_receiver_forget(dsd_state* state) {
    dsd_engine_scan_owner* owner = dsd_engine_scan_owner_get(state);
    if (owner) {
        owner->quiet_warned_reason = (uint8_t)DSD_SCAN_QUIET_REASON_NONE;
    }
}

/* The INPUT detail of a session the check cannot run in. */
static int
quiet_input_detail(const dsd_opts* opts) {
#ifdef USE_RADIO
    if (dsd_opts_input_is_iq_stream(opts)) {
        return SCAN_QUIET_WARN_STREAM;
    }
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        return SCAN_QUIET_WARN_REPLAY; /* any other RTL-family input the session refuses is an I/Q replay */
    }
    return SCAN_QUIET_WARN_AUDIO;
#else
    (void)opts;
    return SCAN_QUIET_WARN_NO_RADIO;
#endif
}

/* Whether the -Y stay on air has carried no call (issue #834 R8, "unsynced HANGTIME"), so its no-check status is shown:
   no call heard since its baseline (dsd_state::scan_y_call_seq: a call the call store began or saw go on, from checked
   content, an admitted analog call, or a ProVoice frame of a confirmed transmission). On a landed row that is the landing (dsd_engine_scan_landing::
   y_call_seq); with no row landing in force (the start frequency before the first step, a step that forgot the landing
   and then failed, an input switch) the stay's own start (dsd_state::scan_y_stay_call_seq). A call explains the stay,
   so no no-check status is shown on it; a check under way still is (R1). Checked content that is no call (an idle
   control channel's traffic) explains nothing, nor does a raw sync, which NXDN48 and dPMR pass on noise several times a
   second, nor one sync of a protocol whose decoder confirms a transmission first (ProVoice), nor raw carrier, which a
   tone-rejected transmission has too, nor the scan's own re-arm of the hangtime anchor (a landing, a hold released). */
static int
quiet_y_visit_unexplained(const dsd_state* state) {
    const dsd_engine_scan_owner* owner = quiet_owner_const(state);
    if (owner && owner->landing.by == DSD_ENGINE_SCAN_LANDING_ROW) {
        return state->scan_y_call_seq == owner->landing.y_call_seq;
    }
    return state->scan_y_call_seq == state->scan_y_stay_call_seq;
}

/* NO_CHECK for @p reason on @p report, when its stay shows the status: on a -Y stay, only while it has carried no
   call. */
static void
quiet_fill_no_check(const dsd_state* state, const scan_quiet_visit_ref* ref, dsd_scan_timing_publication* report,
                    uint8_t reason) {
    if (ref->shows_status && (ref->owner != SCAN_QUIET_OWNER_Y || quiet_y_visit_unexplained(state))) {
        report->quiet_status = (uint8_t)DSD_SCAN_QUIET_STATUS_NO_CHECK;
        report->quiet_reason = reason;
    }
}

#ifdef USE_RADIO

/* The check's realtime clock: a test's, when it set one (DSD_NEO_TEST_HOOKS builds only). */
static uint64_t
quiet_realtime_ms(void) {
#ifdef DSD_NEO_TEST_HOOKS
    if (scan_quiet_test_realtime_ms) {
        return scan_quiet_test_realtime_ms();
    }
#endif
    return dsd_realtime_mono_ms();
}

static int
quiet_revisions_equal(const scan_quiet_revisions* a, const scan_quiet_revisions* b) {
    return a->chan_map_seq == b->chan_map_seq && a->lcn_list_rev == b->lcn_list_rev
           && a->lcn_avoid_rev == b->lcn_avoid_rev && a->trunk_scan_avoid_rev == b->trunk_scan_avoid_rev
           && a->quiet_inv_seq == b->quiet_inv_seq && a->lcn_scan_session == b->lcn_scan_session
           && a->trunk_scan_lists_seq == b->trunk_scan_lists_seq && a->trunk_edit_seq == b->trunk_edit_seq
           && a->y_edits_epoch == b->y_edits_epoch && a->pcm_input_generation == b->pcm_input_generation
           && a->receive_change_seq == b->receive_change_seq && a->nfm_width_hz == b->nfm_width_hz
           && a->am_width_hz == b->am_width_hz;
}

/* The shortened readings a WARN line was logged for (issue #834 follow-up, D3): the setting it was logged under and the
   capture rates, one slot for each rate a check can be made at; a new setting, or a setting transition, starts it
   again. */
typedef struct {
    uint32_t rates_hz[DSD_SIGNAL_QUIET_RATE_COUNT];
    int setting_ms;
    int count;
} quiet_warned_reading;

/* Whether a WARN line for @p setting_ms at @p rate_hz is new to @p w, noting it if so. */
static int
quiet_warned_new(quiet_warned_reading* w, int setting_ms, uint32_t rate_hz) {
    if (w->setting_ms != setting_ms) {
        DSD_MEMSET(w, 0, sizeof *w);
        w->setting_ms = setting_ms;
    }
    for (int i = 0; i < w->count; i++) {
        if (w->rates_hz[i] == rate_hz) {
            return 0;
        }
    }
    /* Only a supported rate is read, so every one has a slot; a full table logs nothing more rather than repeat. */
    if (w->count >= (int)(sizeof w->rates_hz / sizeof w->rates_hz[0])) {
        return 0;
    }
    w->rates_hz[w->count++] = rate_hz;
    return 1;
}

typedef struct {
    scan_quiet_phase phase;
    scan_quiet_visit_key key;
    /* The owner's landing epoch counter when a park still in flight (key.epoch 0) was published: its landing is the
       next epoch, and any other note or forget since ends the check. */
    uint64_t epoch_counter;
    /* As measured: the published classes, width, rate and frequency the request judged. */
    uint8_t classes;
    uint8_t dmr;
    /* The last capture read qualified (quiet_landed_capture()): what the event service waits out a settle on, apart
       from capture_seen, which keeps an in-time sighting for the deadline. */
    uint8_t capture_qualified;
    int analog_kind;
    uint32_t analog_width_hz;
    uint32_t dsp_rate_hz;
    uint32_t frequency_hz;
    scan_quiet_revisions revisions;
    uint32_t generation_before;
    /* The WAIT_CAPTURE bound's origin (realtime): the landing's note, or the in-flight park's completion as the hunt
       first read it; origin_set 0 while that park is still in flight. */
    int origin_set;
    uint64_t origin_ms;
    uint64_t next_capture_check_ms;
    /* The tap's capture word the check last read the capture with (rtl_signal_capture_get_seq()), and whether it read
       one since it entered WAIT_CAPTURE: the event service reads the capture again only once the word moved
       (issue #834 follow-up). */
    uint32_t capture_seq_seen;
    int capture_read;
    /* The shortened reading a WARN line was last logged for (--scan-skip-silent-ms, its sub-window count at the
       capture rate), and the DMR sub-window floor's own line: each once per setting and rate. */
    quiet_warned_reading short_warned;
    quiet_warned_reading dmr_warned;
    /* The frame-boundary settle wait (issue #834 R1): a request a frame-boundary service starts waits
       SCAN_QUIET_FRAME_SETTLE_MS (realtime) from settle_from_ms, the first qualifying capture the service saw (the first
       request) or the retryable failure it collected (a retry); settle_set 0 while there is neither. capture_seen: a
       qualifying capture was seen in this wait before its deadline, so the wait is no longer for a capture and its
       deadline expires it only once no qualifying capture is left. */
    int settle_set;
    int capture_seen;
    uint64_t settle_from_ms;
    /* Suspended while the visit's landing was still in force (issue #834 R5): the next frame boundary publishes it
       again, once (dsd_engine_scan_quiet_take_republish()); a publication or a new start clears it. */
    int republish_owed;
    int retries;
    uint64_t request_seq;
    uint32_t ticket;
    /* A READY result the event service has handed back (or found not passing) once: it is never handed back again by
       events, so a tick that keeps the visit or loses its guard leaves it to the hunt's 300-symbol point, as before. */
    int event_handed;
    uint64_t deadline_ms;
    /* The visit's activity baseline: the activity sequence and the decode time when its tune was issued (a landed
       visit carries them on its landing's note; a park still in flight takes them at its first publication, and keeps
       them when its landing adopts it), kept for that visit across its suspensions, restarts and retries. Checked
       content, a carrier or allowed activity since then ends the visit's check, whatever phase the check was in.
       visit_set 0 before the visit's first publication. */
    int visit_set;
    scan_quiet_visit_key visit_key;
    uint64_t activity_seq;
    double visit_decode_m;
    uint64_t ready_ms;
    dsd_signal_quiet_result result;
    /* A refusal of the visit's capture (a centred capture, an uncalibrated rate or analog width; R10): the check stays
       SUSPENDED, so an edit restarts it on the same visit, but a publication of the same facts and revisions while the
       stream keeps the generation it refused (refused_generation, which any change of rate, shift or receive profile
       moves) starts nothing: one compare, no capture read. */
    int refused;
    uint32_t refused_generation;
    /* The stream generation a refusal's republication was last owed for (dsd_engine_scan_quiet_take_republish()): a
       generation other than the refused one owes one, once, whatever the revisions say. */
    uint32_t refused_republish_generation;
    /* Why the visit gets no check (dsd_scan_quiet_reason), with the check SUSPENDED (issue #834 R6): its session's or
       its facts' reason at its publication, or its capture's (a refusal). NONE for any other phase or suspension, but a
       suspension that activity ended (suspension_kept). */
    uint8_t reason;
    /* A -Y visit's check was SUSPENDED when activity heard on the visit ended it (DONE): the suspension's reason stays,
       for the visit's no-check status only, while the facts it was found on stand (revisions). Activity is no call: a
       short carrier that no tone check admitted still leaves the visit unchecked, and the status hides only once a call
       explains the visit (quiet_fill_no_check()). */
    int suspension_kept;
    /* What the last capture read showed of the receiver (TUNER, RATE, or NONE for one it checks), on the input it was
       read on (receiver_input, receiver_device): the session's reason, which outranks a channel's, for a visit that
       never reads a capture (no class to judge) or still waits for its own. */
    uint8_t receiver_reason;
    int receiver_input;
    uint32_t receiver_device;
    uint32_t receiver_rate_hz;
    /* The DSP rate the visit it was read on ran at (a list's DSP bandwidth sets the capture rate), and the stream
       session it was read in (rtl_stream_session_generation(): the offset tuning, centred I/Q and shift settings that
       decide the rotation are set when a stream opens, never per tune). */
    uint32_t receiver_dsp_rate_hz;
    uint32_t receiver_session;
    /* The revisions a no-check suspension was last found moved at (dsd_engine_scan_quiet_take_republish()), so a frame
       boundary owes one republication per change. */
    scan_quiet_revisions republish_seen;
    /* The channel WARN keys logged, per row or target (issue #834 R6): [0] the trunk scan's, [1] -Y's. */
    struct quiet_channel_key* warned[2];
    size_t warned_cap[2];
} scan_quiet_check;

/* A channel's WARN key: its list and the list's revision, its frequency, reason, kind, width, classes, and the decoder
   set (scan_quiet_channel_detail()) and -8 monitor width in force. */
typedef struct quiet_channel_key {
    uint64_t list_id;
    uint64_t list_rev;
    uint32_t frequency_hz;
    uint8_t reason;
    uint8_t classes;
    uint8_t detail;
    int kind;
    uint32_t width_hz;
    int monitor_width_hz; /* the -8 monitor's configured width; -1 without it */
} quiet_channel_key;

static void
quiet_check_free(void* ptr) {
    scan_quiet_check* c = (scan_quiet_check*)ptr;
    if (c && c->ticket != 0U) {
        rtl_signal_quiet_cancel(c->ticket);
    }
    if (c) {
        free(c->warned[0]);
        free(c->warned[1]);
    }
    free(c);
}

static scan_quiet_check*
quiet_check_get(dsd_engine_scan_owner* owner) {
    return owner ? (scan_quiet_check*)owner->quiet : NULL;
}

static scan_quiet_check*
quiet_check_ensure(dsd_engine_scan_owner* owner) {
    if (!owner->quiet) {
        owner->quiet = calloc(1, sizeof(scan_quiet_check));
        owner->quiet_free = owner->quiet ? quiet_check_free : NULL;
    }
    return (scan_quiet_check*)owner->quiet;
}

/* Let go of the tap's request, if one is outstanding, and end in @p phase, with no reason given yet. */
static void
quiet_end(scan_quiet_check* c, scan_quiet_phase phase) {
    if (c->ticket != 0U) {
        rtl_signal_quiet_cancel(c->ticket);
        c->ticket = 0U;
    }
    c->phase = phase;
    c->reason = (uint8_t)DSD_SCAN_QUIET_REASON_NONE;
    c->suspension_kept = 0;
    c->refused = 0;
}

static int
quiet_check_active(const scan_quiet_check* c) {
    return c->phase == SCAN_QUIET_WAIT_CAPTURE || c->phase == SCAN_QUIET_REQUESTED || c->phase == SCAN_QUIET_READY;
}

static int
quiet_key_equal(const scan_quiet_visit_key* a, const scan_quiet_visit_key* b) {
    return a->owner == b->owner && a->index == b->index && a->session == b->session && a->epoch == b->epoch
           && a->tune_request == b->tune_request;
}

/* Activity heard on the visit @p key ends its check (DONE). A -Y visit's own suspension keeps its reason for the visit's
   status (scan_quiet_check::suspension_kept), which a call heard on the stay hides. A trunk target's status shows its
   suspension only, as before. */
static void
quiet_end_by_activity(scan_quiet_check* c, const scan_quiet_visit_key* key) {
    const int suspended =
        c->phase == SCAN_QUIET_SUSPENDED && c->key.owner == SCAN_QUIET_OWNER_Y && quiet_key_equal(&c->key, key);
    const uint8_t reason = c->reason;
    quiet_end(c, SCAN_QUIET_DONE);
    if (suspended) {
        c->suspension_kept = 1;
        c->reason = reason;
    }
}

static uint8_t
quiet_landing_by(int owner) {
    return (uint8_t)(owner == SCAN_QUIET_OWNER_TRUNK ? DSD_ENGINE_SCAN_LANDING_TARGET : DSD_ENGINE_SCAN_LANDING_ROW);
}

/* Whether @p landing is the one the in-flight park of @p c lands: its row or target, session, tune and frequency, and
   the very next epoch after the check was published. */
static int
quiet_landing_adopts(const scan_quiet_check* c, const dsd_engine_scan_landing* landing) {
    return landing->by == quiet_landing_by(c->key.owner) && landing->index == c->key.index
           && landing->session == c->key.session && landing->tune_request == c->key.tune_request
           && landing->hz == c->frequency_hz && landing->epoch == c->epoch_counter + 1U;
}

/* Whether the visit @p c measures is still the one on air: its landing, or its park still in flight, or that park's
   own landing. */
static int
quiet_visit_on_air(const dsd_engine_scan_owner* owner, const scan_quiet_check* c) {
    const dsd_engine_scan_landing* landing = &owner->landing;
    if (c->key.epoch != 0U) {
        return landing->by == quiet_landing_by(c->key.owner) && landing->epoch == c->key.epoch;
    }
    if (landing->by == DSD_ENGINE_SCAN_LANDING_NONE) {
        return owner->landing_epoch == c->epoch_counter;
    }
    return quiet_landing_adopts(c, landing);
}

/* Whether the visit @p c measures is still the one on air; a park in flight it measured is adopted (@p adopt) once
   its own landing is noted. */
static int
quiet_visit_current(const dsd_engine_scan_owner* owner, scan_quiet_check* c, int adopt) {
    if (!quiet_visit_on_air(owner, c)) {
        return 0;
    }
    const dsd_engine_scan_landing* landing = &owner->landing;
    if (adopt && c->key.epoch == 0U && landing->by != DSD_ENGINE_SCAN_LANDING_NONE) {
        if (c->visit_set && quiet_key_equal(&c->visit_key, &c->key)) {
            c->visit_key.epoch = landing->epoch; /* the park's landing is the same visit */
        }
        c->key.epoch = landing->epoch;
        if (!c->origin_set) {
            /* The hunt never read the park's completion (another landing succeeded first): the wait is bounded from
               the landing's note. */
            c->origin_set = 1;
            c->origin_ms = landing->landed_realtime_ms;
        }
    }
    return 1;
}

static uint64_t
quiet_age_ms(uint64_t now_ms, uint64_t since_ms) {
    return now_ms >= since_ms ? now_ms - since_ms : 0U;
}

/* Whether a frame's verdict is still awaited (issue #834 R1): a sync whose frame has not ended, or a PARTIAL frame's
   hold not over yet (both sides on the decode clock). A quiet result is neither handed back, applied nor declined while
   one is; the check's reconcile, collection and deadlines go on. */
static int
quiet_pending(const dsd_engine_scan_owner* owner) {
    if (owner->frame_pending) {
        return 1;
    }
    return owner->pending_until_m > 0.0 && dsd_decode_now_mono_s() < owner->pending_until_m;
}

/* Whether checked content or a carrier was heard since the visit's activity baseline. */
static int
quiet_visit_heard_activity(const dsd_engine_scan_owner* owner, const scan_quiet_check* c) {
    return c->visit_set && quiet_key_equal(&c->visit_key, &c->key) && owner->activity_seq != c->activity_seq;
}

/* Reconcile: a changed visit or revision suspends the check; activity since the visit's activity baseline, or a READY
   result past its age, ends it. A pending frame verdict ends nothing: a READY result it holds back still ages out. */
static void
quiet_reconcile_visit(const dsd_opts* opts, const dsd_state* state, const dsd_engine_scan_owner* owner,
                      scan_quiet_check* c) {
    if (!quiet_check_active(c)) {
        return;
    }
    scan_quiet_revisions now_rev;
    scan_quiet_revisions_read(opts, state, &now_rev);
    const int current = quiet_visit_current(owner, c, 1);
    if (!current || !quiet_revisions_equal(&now_rev, &c->revisions)) {
        quiet_end(c, SCAN_QUIET_SUSPENDED);
        c->republish_owed = current;
        return;
    }
    if (quiet_visit_heard_activity(owner, c)) {
        quiet_end(c, SCAN_QUIET_DONE);
    }
}

/* Whether the READY result of @p c is past SCAN_QUIET_READY_MAX_AGE_MS (realtime). */
static int
quiet_ready_aged(const scan_quiet_check* c, uint64_t now_ms) {
    return c->phase == SCAN_QUIET_READY && quiet_age_ms(now_ms, c->ready_ms) > SCAN_QUIET_READY_MAX_AGE_MS;
}

/* The legacy reconcile: quiet_reconcile_visit(), and a READY result past its age ends the check. The event service
   reconciles the visit only and leaves that expiry here (issue #834 follow-up). */
static void
quiet_reconcile(const dsd_opts* opts, const dsd_state* state, const dsd_engine_scan_owner* owner, scan_quiet_check* c,
                uint64_t now_ms) {
    quiet_reconcile_visit(opts, state, owner, c);
    if (quiet_ready_aged(c, now_ms)) {
        c->phase = SCAN_QUIET_DONE;
    }
}

/* The capture of the landed channel, or 0 while there is none yet: a capture of the stream's live output generation,
   newer than the one read at the tune request, centred where the visit landed. */
static int
quiet_landed_capture(scan_quiet_check* c, dsd_signal_capture* cap) {
    c->capture_read = 1;
    c->capture_qualified = 0;
    if (!rtl_signal_capture_get_seq(cap, &c->capture_seq_seen)) {
        return 0;
    }
    c->capture_qualified = cap->generation != 0U && cap->generation == dsd_rtl_stream_metrics_hook_stream_generation()
                           && cap->generation != c->generation_before
                           && (int64_t)cap->capture_hz - cap->capture_offset_hz == (int64_t)c->frequency_hz;
    return c->capture_qualified;
}

/* The receiver's part of what the calibration refuses of the capture: exactly one of its two geometry policies is
   covered, the default one with the Fs/4 shift (rotation a quarter of the rate, the channel that far from the hardware
   centre) or a native Airspy's centred capture (tuned on the channel: no rotation, no offset). Anything else (centred
   I/Q under the default policy, an offset-tuned E4000, the shift off, any other rotation or an unknown policy) is
   TUNER; a covered geometry at a rate its policy did not validate is RATE, and so is a SoapySDR Airspy at any rate. */
static uint8_t
quiet_capture_receiver_reason(const dsd_signal_capture* cap) {
    if (cap->geometry == DSD_SIGNAL_GEOMETRY_DEFAULT) {
        if (cap->rate_hz == 0U || cap->rotation_hz != (int32_t)(cap->rate_hz / 4U)
            || cap->capture_offset_hz != cap->rotation_hz) {
            return (uint8_t)DSD_SCAN_QUIET_REASON_TUNER;
        }
        /* A SoapySDR Airspy at any rate, or a SoapySDR device that cannot be ruled out as one at an Airspy rate. */
        if (dsd_signal_quiet_receiver_refused(cap)) {
            return (uint8_t)DSD_SCAN_QUIET_REASON_RATE;
        }
        return dsd_signal_quiet_rate_supported(cap->rate_hz) ? (uint8_t)DSD_SCAN_QUIET_REASON_NONE
                                                             : (uint8_t)DSD_SCAN_QUIET_REASON_RATE;
    }
    if (cap->geometry == DSD_SIGNAL_GEOMETRY_AIRSPY_CENTRED) {
        if (cap->rotation_hz != 0 || cap->capture_offset_hz != 0) {
            return (uint8_t)DSD_SCAN_QUIET_REASON_TUNER;
        }
        return dsd_signal_quiet_airspy_centred_rate_supported(cap->rate_hz) ? (uint8_t)DSD_SCAN_QUIET_REASON_NONE
                                                                            : (uint8_t)DSD_SCAN_QUIET_REASON_RATE;
    }
    return (uint8_t)DSD_SCAN_QUIET_REASON_TUNER;
}

/* What the calibration refuses of the capture (dsd_scan_quiet_reason): the receiver's part
   (quiet_capture_receiver_reason(): TUNER for a geometry it does not cover, RATE for a rate it did not validate under
   the capture's policy), then WIDTH for an analog width it did not validate for its kind there; NONE when it covers
   all three. The receiver's part is kept as the session's verdict, with the input it was read on. */
static uint8_t
quiet_capture_refusal(const dsd_opts* opts, scan_quiet_check* c, const dsd_signal_capture* cap) {
    const uint8_t receiver = quiet_capture_receiver_reason(cap);
    c->receiver_reason = receiver;
    c->receiver_input = opts->audio_in_type;
    c->receiver_device = quiet_device_hash(opts);
    c->receiver_rate_hz = cap->rate_hz;
    c->receiver_dsp_rate_hz = c->dsp_rate_hz;
    c->receiver_session = rtl_stream_session_generation();
    if (receiver != (uint8_t)DSD_SCAN_QUIET_REASON_NONE) {
        return receiver;
    }
    if ((c->classes & QUIET_CLASS_ANALOG)
        && !dsd_signal_quiet_analog_supported(c->analog_kind, c->analog_width_hz, cap->rate_hz)) {
        return (uint8_t)DSD_SCAN_QUIET_REASON_WIDTH;
    }
    return (uint8_t)DSD_SCAN_QUIET_REASON_NONE;
}

/* The capture refused the visit for @p reason: SUSPENDED, not DONE, so an edit that changes what it refused starts it
   again on the same visit (issue #834 R10); until then the refusal is cached (scan_quiet_check::refused) against
   @p generation, the stream generation of what was refused: the refused capture's own, never one read later, since the
   RTL thread may open a new generation (rotation back on) between the capture's validation and its refusal. */
static void
quiet_refuse(scan_quiet_check* c, uint8_t reason, uint32_t generation) {
    quiet_end(c, SCAN_QUIET_SUSPENDED);
    c->reason = reason;
    c->refused = 1;
    c->refused_generation = generation;
    c->refused_republish_generation = generation;
    c->republish_owed = 0;
}

/* The length of one sub-window at @p rate_hz for a narrowest judged width of @p width_hz, in ms (the detector's rule:
   the smallest whole number of transforms of at least 10 ms); 0 when no transform fits. */
static double
quiet_window_ms(uint32_t rate_hz, double width_hz) {
    const int n = dsd_signal_fft_size_for_width(rate_hz, width_hz);
    if (n <= 0 || rate_hz == 0U) {
        return 0.0;
    }
    const uint64_t frames = ((uint64_t)rate_hz + 100U * (uint64_t)n - 1U) / (100U * (uint64_t)n);
    return 1000.0 * (double)(frames * (uint64_t)n) / (double)rate_hz;
}

/* The shortest and longest sub-window any checked width has at @p rate_hz, in ms. */
static void
quiet_window_range_ms(uint32_t rate_hz, double* lo, double* hi) {
    *lo = *hi = quiet_window_ms(rate_hz, DSD_SIGNAL_QUIET_WIDTH_12K5_HZ);
    const double digital = quiet_window_ms(rate_hz, DSD_SIGNAL_QUIET_WIDTH_6K25_HZ);
    *lo = digital < *lo ? digital : *lo;
    *hi = digital > *hi ? digital : *hi;
    for (int kind = 0; kind < 2; kind++) {
        size_t count = 0;
        const uint32_t* widths = scan_quiet_warn_widths(kind ? DSD_ANALOG_DEMOD_AM : DSD_ANALOG_DEMOD_FM, &count);
        for (size_t i = 0; i < count; i++) {
            const double ms = quiet_window_ms(rate_hz, (double)widths[i]);
            *lo = ms < *lo ? ms : *lo;
            *hi = ms > *hi ? ms : *hi;
        }
    }
}

int
scan_quiet_format_short_warn(char* out, size_t out_size, int setting_ms, uint32_t windows, uint32_t rate_hz,
                             int dmr_floor) {
    double lo = 0.0;
    double hi = 0.0;
    quiet_window_range_ms(rate_hz, &lo, &hi);
    /* The reading's span over every checked width, in whole ms: one figure when both ends print the same. */
    const long first = lround(lo * windows);
    const long last = lround(hi * windows);
    char span[48];
    if (first == last) {
        DSD_SNPRINTF(span, sizeof span, "%ld ms", first);
    } else {
        DSD_SNPRINTF(span, sizeof span, "%ld to %ld ms", first, last);
    }
    if (dmr_floor) {
        return DSD_SNPRINTF(out, out_size,
                            "WARNING: scan: --scan-skip-silent-ms %d: DMR channels are read for %u slices (%s), the "
                            "fewest that always hold one whole slice inside a burst of an ongoing DMR transmission.\n",
                            setting_ms, (unsigned)windows, span);
    }
    return DSD_SNPRINTF(out, out_size,
                        "WARNING: scan: --scan-skip-silent-ms %d reads %u slices (%s), shorter than the calibrated "
                        "9: a weak or fading signal can read silent and be skipped.\n",
                        setting_ms, (unsigned)windows, span);
}

/* The WARN lines a shortened reading takes (issue #834 follow-up, D3): once per setting and capture rate for the
   reading, and once more for the DMR floor when a DMR visit is read for longer than the setting asks. */
static void
quiet_warn_short(const dsd_opts* opts, scan_quiet_check* c, uint32_t rate_hz, uint32_t setting, uint32_t windows) {
    if (setting == 0U || setting >= 9U) {
        return;
    }
    char line[320];
    if (quiet_warned_new(&c->short_warned, opts->scan_skip_silent_ms, rate_hz)) {
        (void)scan_quiet_format_short_warn(line, sizeof line, opts->scan_skip_silent_ms, setting, rate_hz, 0);
        LOG_WARN("%s", line);
    }
    if (windows > setting && quiet_warned_new(&c->dmr_warned, opts->scan_skip_silent_ms, rate_hz)) {
        (void)scan_quiet_format_short_warn(line, sizeof line, opts->scan_skip_silent_ms, windows, rate_hz, 1);
        LOG_WARN("%s", line);
    }
}

static void
quiet_request(const dsd_opts* opts, scan_quiet_check* c, const dsd_signal_capture* cap, uint64_t now_ms) {
    dsd_signal_quiet_request request;
    DSD_MEMSET(&request, 0, sizeof request);
    request.request_id = ++c->request_seq;
    request.capture = *cap;
    request.frequency_hz = c->frequency_hz;
    request.classes = c->classes;
    request.analog_width_hz = (c->classes & QUIET_CLASS_ANALOG) ? c->analog_width_hz : 0U;
    /* The reading starts SCAN_QUIET_LEAD_MS of samples after the request's capture (issue #834 follow-up, D1-a). */
    request.lead_pairs = (uint32_t)((uint64_t)cap->rate_hz * SCAN_QUIET_LEAD_MS / 1000U);
    /* The reading's length (issue #834 follow-up, D3): the calibrated one unless --scan-skip-silent-ms sets another,
       never fewer than the DMR floor on a visit that can carry DMR. */
    const uint32_t setting = scan_quiet_setting_windows(opts);
    request.windows = scan_quiet_request_windows(setting, c->dmr);
    quiet_warn_short(opts, c, cap->rate_hz, setting, request.windows);
    uint32_t ticket = 0U;
    if (rtl_signal_quiet_request(&request, &ticket, NULL) != 0) {
        /* Refused (a Signal search holds the tap, or no memory): no check this visit. */
        c->phase = SCAN_QUIET_DONE;
        return;
    }
    c->ticket = ticket;
    c->deadline_ms = now_ms + scan_quiet_request_deadline_ms(request.windows);
    c->phase = SCAN_QUIET_REQUESTED;
}

/* The WAIT_CAPTURE deadline: 1000 ms from its origin, the bound on obtaining a qualifying capture. Where the
   frame-boundary settle wait runs (a frame-boundary call, @p at_frame, or a capture already seen), it is
   max(origin + 1000 ms, settle start + SCAN_QUIET_FRAME_SETTLE_MS), so waiting out the settle never expires a check. A
   legacy opportunity with no capture seen keeps the plain bound: a visit with no frame-boundary service times every
   request and expiry exactly as before. */
static uint64_t
quiet_capture_deadline_ms(const scan_quiet_check* c, int at_frame) {
    const uint64_t bound_ms = c->origin_ms + SCAN_QUIET_CAPTURE_WAIT_MS;
    if (!c->settle_set || !(at_frame || c->capture_seen)) {
        return bound_ms;
    }
    const uint64_t settled_ms = c->settle_from_ms + SCAN_QUIET_FRAME_SETTLE_MS;
    return settled_ms > bound_ms ? settled_ms : bound_ms;
}

/* WAIT_CAPTURE, from a legacy opportunity (the hunt's 300-symbol point, an engine publication: @p at_frame 0) or a
   frame boundary (@p at_frame 1). A legacy call reads the capture at most every 50 ms and requests as soon as it finds
   a qualifying one. A frame-boundary call reads it without moving that schedule and requests only once the settle wait
   has run since the first qualifying capture the service saw (or since the retryable failure, for a retry). */
static void
quiet_wait_capture(const dsd_opts* opts, scan_quiet_check* c, uint64_t now_ms, int at_frame) {
    if (!c->origin_set) {
        /* A park still in flight: its wait starts once the tuning ledger published its completion. */
        if (!dsd_trunk_tuning_request_landed(c->key.tune_request)) {
            return;
        }
        c->origin_set = 1;
        c->origin_ms = now_ms;
    }
    const int expired = now_ms >= quiet_capture_deadline_ms(c, at_frame);
    if (expired && !c->capture_seen) {
        c->phase = SCAN_QUIET_DONE;
        return;
    }
    if (!at_frame) {
        if (now_ms < c->next_capture_check_ms) {
            return;
        }
        c->next_capture_check_ms = now_ms + SCAN_QUIET_CAPTURE_RECHECK_MS;
    }
    dsd_signal_capture cap;
    if (!quiet_landed_capture(c, &cap)) {
        if (expired) {
            c->phase = SCAN_QUIET_DONE; /* the capture seen is gone, past the deadline */
        }
        return;
    }
    if (!c->capture_seen) {
        c->capture_seen = 1;
        if (!c->settle_set) {
            c->settle_set = 1;
            c->settle_from_ms = now_ms;
        }
    }
    if (at_frame && quiet_age_ms(now_ms, c->settle_from_ms) < SCAN_QUIET_FRAME_SETTLE_MS) {
        return;
    }
    const uint8_t refusal = quiet_capture_refusal(opts, c, &cap);
    if (refusal != (uint8_t)DSD_SCAN_QUIET_REASON_NONE) {
        quiet_refuse(c, refusal, cap.generation);
        return;
    }
    quiet_request(opts, c, &cap, now_ms);
}

static int
quiet_failure_retries(dsd_signal_measure_failure failure) {
    return failure == DSD_SIGNAL_MEASURE_SUPERSEDED || failure == DSD_SIGNAL_MEASURE_CAPTURE_CHANGED
           || failure == DSD_SIGNAL_MEASURE_DROPPED;
}

static void
quiet_collect(scan_quiet_check* c, uint64_t now_ms, int keep_schedule) {
    dsd_signal_quiet_result result;
    DSD_MEMSET(&result, 0, sizeof result);
    const int taken = rtl_signal_quiet_status(c->ticket) == 0 ? 0 : rtl_signal_quiet_take(c->ticket, &result);
    if (taken == 0) {
        if (now_ms >= c->deadline_ms) {
            quiet_end(c, SCAN_QUIET_DONE);
        }
        return;
    }
    c->ticket = 0U;
    /* The answer bound holds for an answer too: one that completed past the deadline (on the realtime clock the
       deadline is on; the poll that finds it when it carries no completion time) is no answer in time, whatever it
       says, and ends the visit as the timeout does, with no retry. */
    const uint64_t completed_ms = result.completed_realtime_ns != 0U ? result.completed_realtime_ns / 1000000U : now_ms;
    if (completed_ms >= c->deadline_ms) {
        c->phase = SCAN_QUIET_DONE;
        return;
    }
    if (taken > 0) {
        /* A quiet answer found after it aged past SCAN_QUIET_READY_MAX_AGE_MS is as stale as a READY one the reconcile
           ages out: no skip, and no hand-back that would leave the frame loop for a skip the apply refuses. */
        if (result.request_id == c->request_seq && result.valid && result.quiet
            && quiet_age_ms(now_ms, completed_ms) <= SCAN_QUIET_READY_MAX_AGE_MS) {
            c->result = result;
            c->ready_ms = completed_ms;
            c->event_handed = 0;
            c->phase = SCAN_QUIET_READY;
        } else {
            c->phase = SCAN_QUIET_DONE; /* busy, not measurable, or stale */
        }
        return;
    }
    if (quiet_failure_retries(result.failure) && c->retries < SCAN_QUIET_MAX_RETRIES) {
        c->retries++;
        if (!keep_schedule) {
            /* A legacy pickup reads the capture again at its next opportunity; an event's keeps the 50 ms schedule. */
            c->next_capture_check_ms = 0U;
        }
        /* A retry started at a frame boundary waits out the settle from this failure. */
        c->settle_set = 1;
        c->settle_from_ms = now_ms;
        c->capture_seen = 0;
        c->capture_read = 0;
        c->capture_qualified = 0;
        c->phase = SCAN_QUIET_WAIT_CAPTURE;
        return;
    }
    c->phase = SCAN_QUIET_DONE;
}

/* Start, collect or expire, by phase; @p at_frame for a frame-boundary call (quiet_wait_capture()). */
static void
quiet_step(const dsd_opts* opts, scan_quiet_check* c, uint64_t now_ms, int at_frame) {
    if (c->phase == SCAN_QUIET_WAIT_CAPTURE) {
        quiet_wait_capture(opts, c, now_ms, at_frame);
    } else if (c->phase == SCAN_QUIET_REQUESTED) {
        quiet_collect(c, now_ms, 0);
    }
}

static int
quiet_capture_equal(const dsd_signal_capture* a, const dsd_signal_capture* b) {
    return a->generation == b->generation && a->rate_hz == b->rate_hz && a->capture_hz == b->capture_hz
           && a->capture_offset_hz == b->capture_offset_hz && a->rotation_hz == b->rotation_hz && a->gain == b->gain
           && a->drops == b->drops && a->geometry == b->geometry && a->receiver == b->receiver;
}

/* For a reconciled READY result past the class and revision checks: the tap's capture and the live output
   generation are still the result's, no tune is pending, no analog carrier is heard and, under trunk scan, no allowed
   activity came since the visit's activity baseline. */
static int
quiet_ready_passes(const dsd_opts* opts, const dsd_state* state, const scan_quiet_check* c) {
    dsd_signal_capture cap;
    if (!rtl_signal_capture_get(&cap) || !quiet_capture_equal(&cap, &c->result.capture)
        || cap.generation != dsd_rtl_stream_metrics_hook_stream_generation()) {
        return 0;
    }
    /* Lock-free: no tune in flight or failed-gated anywhere in the ledger. */
    if (!dsd_trunk_tuning_frame_is_current(dsd_trunk_tuning_generation())) {
        return 0;
    }
    /* An analog carrier heard, whatever its tone verdict. */
    if (dsd_scan_analog_carrier_open(opts, state)
        || dsd_scan_analog_tone_gate(opts, state) != DSD_ANALOG_TONE_GATE_OFF) {
        return 0;
    }
    return c->key.owner != SCAN_QUIET_OWNER_TRUNK
           || !dsd_engine_trunk_scan_quiet_activity_since(state, c->visit_decode_m);
}

int
scan_quiet_session_supported(const dsd_opts* opts) {
    return opts && opts->scan_skip_silent && dsd_opts_radio_can_tune(opts)
           && dsd_decode_clock_source() != DSD_DECODE_CLOCK_REPLAY;
}

int
scan_quiet_wants_eligibility(const dsd_opts* opts, const dsd_state* state, int owner) {
    if (!opts || !opts->scan_skip_silent) {
        return 0;
    }
    const dsd_engine_scan_owner* scan_owner = quiet_owner_const(state);
    const scan_quiet_check* c = scan_owner ? (const scan_quiet_check*)scan_owner->quiet : NULL;
    if (!c || c->phase != SCAN_QUIET_DONE || c->key.owner != owner) {
        return 1;
    }
    scan_quiet_check probe = *c;
    return !quiet_visit_current(scan_owner, &probe, 0);
}

/* Whether @p e can be measured: the owner's conditions, the session's, a class to judge, and a key that is the visit
   on air (a landed one) or a park in flight (no landing). */
static int
quiet_publication_eligible(const dsd_opts* opts, const dsd_engine_scan_owner* owner, int owner_kind,
                           const scan_quiet_eligibility* e) {
    if (!e->eligible || e->key.owner != owner_kind || !scan_quiet_session_supported(opts) || e->classes == 0U
        || e->frequency_hz == 0U || ((e->classes & QUIET_CLASS_ANALOG) && e->analog_width_hz == 0U)) {
        return 0;
    }
    const dsd_engine_scan_landing* landing = &owner->landing;
    if (e->key.epoch == 0U) {
        return owner_kind == SCAN_QUIET_OWNER_TRUNK && e->key.tune_request != 0U
               && landing->by == DSD_ENGINE_SCAN_LANDING_NONE;
    }
    return landing->by == quiet_landing_by(owner_kind) && landing->epoch == e->key.epoch
           && landing->index == e->key.index && landing->session == e->key.session && landing->hz == e->frequency_hz;
}

static int
quiet_published_as_measured(const scan_quiet_check* c, const scan_quiet_eligibility* e) {
    return quiet_key_equal(&c->key, &e->key) && c->classes == e->classes && c->analog_kind == e->analog_kind
           && c->analog_width_hz == e->analog_width_hz && c->dsp_rate_hz == e->dsp_rate_hz
           && c->frequency_hz == e->frequency_hz && c->dmr == e->dmr
           && quiet_revisions_equal(&c->revisions, &e->revisions);
}

/* The effective classes, the analog kind and width (with the analog class) and the DSP rate in force
   (@p current, this pass's facts) are still the ones measured. */
static int
quiet_measured_as_current(const scan_quiet_check* c, const scan_quiet_eligibility* current) {
    if (!current || current->classes != c->classes || current->dsp_rate_hz != c->dsp_rate_hz
        || current->dmr != c->dmr) {
        return 0;
    }
    return !(c->classes & QUIET_CLASS_ANALOG)
           || (current->analog_kind == c->analog_kind && current->analog_width_hz == c->analog_width_hz);
}

static void
quiet_start(const dsd_engine_scan_owner* owner, scan_quiet_check* c, const scan_quiet_eligibility* e, uint64_t now_ms) {
    const int landed = e->key.epoch != 0U;
    /* A landed visit's first check waits for its capture from the landing's note. One that starts again on a visit
       already published (after a suspension, eligible only now, or with other facts) waits from now: the stream may
       only now make the capture it needs (a receive-profile or width change moves the output generation). */
    const int restart = landed && quiet_key_equal(&c->key, &e->key);
    quiet_end(c, SCAN_QUIET_WAIT_CAPTURE);
    c->key = e->key;
    c->epoch_counter = owner->landing_epoch;
    c->classes = e->classes;
    c->analog_kind = e->analog_kind;
    c->analog_width_hz = e->analog_width_hz;
    c->dsp_rate_hz = e->dsp_rate_hz;
    c->frequency_hz = e->frequency_hz;
    c->dmr = e->dmr;
    c->revisions = e->revisions;
    c->republish_seen = e->revisions;
    c->generation_before = landed ? owner->landing.generation_before_tune : owner->tune_generation_before;
    c->origin_set = landed;
    c->origin_ms = restart ? now_ms : (landed ? owner->landing.landed_realtime_ms : 0U);
    c->next_capture_check_ms = 0U;
    c->capture_read = 0;
    c->capture_qualified = 0;
    c->settle_set = 0;
    c->settle_from_ms = 0U;
    c->capture_seen = 0;
    c->republish_owed = 0;
    c->retries = 0;
}

/* The visit's activity baseline for @p key: its tune's issue, which its landing's note carries, or which a park still
   in flight reads at its first publication. A later publication of the same visit keeps it, so what was heard while
   its check was suspended still counts. */
static void
quiet_visit_baseline(const dsd_engine_scan_owner* owner, scan_quiet_check* c, const scan_quiet_visit_key* key) {
    if (c->visit_set && quiet_key_equal(&c->visit_key, key)) {
        return;
    }
    c->visit_set = 1;
    c->visit_key = *key;
    const dsd_engine_scan_landing* landing = &owner->landing;
    if (key->epoch != 0U && landing->by == quiet_landing_by(key->owner) && landing->epoch == key->epoch) {
        /* A landed visit counts from its tune's issue, which its landing's note carries. */
        c->activity_seq = landing->activity_seq;
        c->visit_decode_m = landing->tune_decode_m;
        return;
    }
    /* A park still in flight counts from its tune's issue, the scan's last tune request. */
    c->activity_seq = owner->tune_activity_seq;
    c->visit_decode_m = owner->tune_decode_m;
}

/* The scan tune that put @p key's visit on air (dsd_engine_scan_owner::tune_seq): its landing's, or for a park still
   in flight, or one whose landing is not noted yet, the scan's last. */
static uint64_t
quiet_visit_tune_seq(const dsd_engine_scan_owner* owner, const scan_quiet_visit_key* key) {
    const dsd_engine_scan_landing* landing = &owner->landing;
    if (key->epoch != 0U && landing->by == quiet_landing_by(key->owner) && landing->epoch == key->epoch) {
        return landing->tune_seq;
    }
    return owner->tune_seq;
}

/* A publication's reconcile, and whether @p e's visit has its check ended already: DONE for the visit, a visit tuned
   before the setting was turned on (then DONE now: the setting applies from the next tune), or checked content or a
   carrier heard in it since its activity baseline, its check suspended or not (then DONE now: none is made for it
   again). */
static int
quiet_publication_ends_visit(const dsd_opts* opts, const dsd_state* state, const dsd_engine_scan_owner* owner,
                             scan_quiet_check* c, const scan_quiet_eligibility* e, uint64_t now_ms) {
    quiet_reconcile(opts, state, owner, c, now_ms);
    if ((c->phase == SCAN_QUIET_DONE || c->phase == SCAN_QUIET_SUSPENDED) && c->key.epoch == 0U) {
        /* A park measured or suspended in flight whose landing has been noted: the landing is still its visit. */
        (void)quiet_visit_current(owner, c, 1);
    }
    if (c->phase == SCAN_QUIET_DONE && quiet_key_equal(&c->key, &e->key)) {
        return 1; /* terminal for the visit */
    }
    if (quiet_visit_tune_seq(owner, &e->key) < owner->quiet_first_tune_seq) {
        /* Tuned before the setting turned on: what it heard while off moved nothing. A park in flight keeps what its
           landing adopts it by, as an ineligible one does. */
        quiet_end(c, SCAN_QUIET_DONE);
        c->key = e->key;
        c->epoch_counter = owner->landing_epoch;
        c->frequency_hz = e->frequency_hz;
        return 1;
    }
    quiet_visit_baseline(owner, c, &e->key);
    if (owner->activity_seq == c->activity_seq) {
        return 0;
    }
    quiet_end_by_activity(c, &e->key);
    c->key = e->key;
    return 1;
}

/* An ineligible publication: SUSPENDED, with the reason the visit gets no check in the precedence of the reason
   model (issue #834 R6): the session's, then the facts' (decoders, width); a visit's own conditions (a hold, a stay, no
   alternate, a tune pending) give none. */
static void
quiet_suspend_ineligible(const dsd_opts* opts, const dsd_engine_scan_owner* owner, int owner_kind, scan_quiet_check* c,
                         const scan_quiet_eligibility* e) {
    quiet_end(c, SCAN_QUIET_SUSPENDED);
    c->key = e->key;
    /* What a park in flight is adopted by at its landing, so the visit keeps its baseline even when no check of it
       ever started. */
    c->epoch_counter = owner->landing_epoch;
    c->frequency_hz = e->frequency_hz;
    /* The facts the channel's WARN line names; a start sets them again. */
    c->classes = e->classes;
    c->analog_kind = e->analog_kind;
    c->analog_width_hz = e->analog_width_hz;
    /* What a frame boundary compares to owe its republication once something it depends on moves. */
    c->revisions = e->revisions;
    c->republish_seen = e->revisions;
    if (!scan_quiet_session_supported(opts)) {
        c->reason = (uint8_t)DSD_SCAN_QUIET_REASON_INPUT;
    } else if (e->key.owner == owner_kind) {
        c->reason = e->reason;
    }
}

/* Whether the started check @p c is refused for its analog width without a capture read (issue #834 R10): the last
   capture read, on the input in force, in this stream session (a reopen may turn the rotation off) and at the DSP rate
   this visit runs (so at this visit's capture rate), was rotated and at a validated rate, and the calibration covers
   no such width of this kind there. At another DSP rate the visit reads its own capture, whose rate outranks its
   width. The width table does not hang on the visit, so a width-refused target is refused at once on every visit (no
   Quiet check before its capture comes) until a width, decoder or list edit gives it other facts. */
static int
quiet_width_refused_ahead(const dsd_opts* opts, const scan_quiet_check* c) {
    if (!(c->classes & QUIET_CLASS_ANALOG) || c->receiver_rate_hz == 0U || c->receiver_dsp_rate_hz != c->dsp_rate_hz
        || c->receiver_session != rtl_stream_session_generation()
        || c->receiver_reason != (uint8_t)DSD_SCAN_QUIET_REASON_NONE || c->receiver_input != opts->audio_in_type
        || c->receiver_device != quiet_device_hash(opts) || !dsd_signal_quiet_rate_supported(c->receiver_rate_hz)) {
        return 0;
    }
    return !dsd_signal_quiet_analog_supported(c->analog_kind, c->analog_width_hz, c->receiver_rate_hz);
}

/* Whether a refusal of the visit's capture still stands for @p e (issue #834 R10): the same visit published with the
   same facts and revisions, the stream still on the generation refused. A permanent refusal costs this compare per
   publication: no start, no capture read. */
static int
quiet_refusal_holds(const scan_quiet_check* c, const scan_quiet_eligibility* e) {
    return c->refused && c->phase == SCAN_QUIET_SUSPENDED && quiet_published_as_measured(c, e)
           && c->refused_generation == dsd_rtl_stream_metrics_hook_stream_generation();
}

void
scan_quiet_publish_eligibility(const dsd_opts* opts, dsd_state* state, int owner_kind,
                               const scan_quiet_eligibility* e) {
    if (!opts || !state || !e || !opts->scan_skip_silent) {
        return;
    }
    dsd_engine_scan_owner* owner = dsd_engine_scan_owner_ensure(state);
    scan_quiet_check* c = owner ? quiet_check_ensure(owner) : NULL;
    if (!c) {
        return;
    }
    const uint64_t now_ms = quiet_realtime_ms();
    c->republish_owed = 0; /* this is its publication */
    if (quiet_publication_ends_visit(opts, state, owner, c, e, now_ms)) {
        return;
    }
    if (!quiet_publication_eligible(opts, owner, owner_kind, e)) {
        quiet_suspend_ineligible(opts, owner, owner_kind, c, e);
        return;
    }
    if (quiet_refusal_holds(c, e)) {
        return; /* the capture refused these very facts, and the stream still runs what it refused */
    }
    if (c->phase == SCAN_QUIET_SUSPENDED || !quiet_published_as_measured(c, e)) {
        quiet_start(owner, c, e, now_ms);
        if (quiet_width_refused_ahead(opts, c)) {
            /* Refused on the facts of the stream running now, not on a capture read for this visit. */
            quiet_refuse(c, (uint8_t)DSD_SCAN_QUIET_REASON_WIDTH, dsd_rtl_stream_metrics_hook_stream_generation());
            return;
        }
    }
    quiet_step(opts, c, now_ms, 0);
}

/* The service of a check under way (@p c, reconciled and stepped here), for the hunt or a frame boundary. */
static int
quiet_service(const dsd_opts* opts, const dsd_state* state, const dsd_engine_scan_owner* owner, scan_quiet_check* c,
              int at_frame) {
    const uint64_t now_ms = quiet_realtime_ms();
    quiet_reconcile(opts, state, owner, c, now_ms);
    quiet_step(opts, c, now_ms, at_frame);
    if (!quiet_check_active(c)) {
        return DSD_SCAN_QUIET_NONE;
    }
    /* A READY result a frame's pending verdict holds back is LIVE: it stays READY, and still ages out. */
    if (c->phase == SCAN_QUIET_READY && !quiet_pending(owner) && quiet_ready_passes(opts, state, c)) {
        return DSD_SCAN_QUIET_HAND_BACK;
    }
    return DSD_SCAN_QUIET_LIVE;
}

int
dsd_engine_scan_quiet_service(const dsd_opts* opts, dsd_state* state) {
    dsd_engine_scan_owner* owner = dsd_engine_scan_owner_get(state);
    scan_quiet_check* c = quiet_check_get(owner);
    if (!c || c->phase == SCAN_QUIET_SUSPENDED || c->phase == SCAN_QUIET_DONE) {
        return DSD_SCAN_QUIET_NONE;
    }
    return quiet_service(opts, state, owner, c, 0);
}

int
dsd_engine_scan_quiet_frame_service(const dsd_opts* opts, dsd_state* state, int owner_kind) {
    /* The setting before anything else: with it off, the null check and one load. */
    if (!opts || !opts->scan_skip_silent) {
        return DSD_SCAN_QUIET_NONE;
    }
    dsd_engine_scan_owner* owner = dsd_engine_scan_owner_get(state);
    scan_quiet_check* c = quiet_check_get(owner);
    if (!c || c->key.owner != owner_kind || c->phase == SCAN_QUIET_SUSPENDED || c->phase == SCAN_QUIET_DONE) {
        return DSD_SCAN_QUIET_NONE;
    }
    return quiet_service(opts, state, owner, c, 1);
}

int
dsd_engine_scan_quiet_frame_yield(const dsd_opts* opts, dsd_state* state) {
    /* The setting before anything else: with it off, the null check and one load. */
    return opts && opts->scan_skip_silent && opts->scanner_mode == 1
           && dsd_engine_scan_quiet_frame_service(opts, state, SCAN_QUIET_OWNER_Y) == DSD_SCAN_QUIET_HAND_BACK;
}

/* WAIT_CAPTURE from the event service (issue #834 follow-up): as a frame boundary may, never earlier, and never ending
   the check. A park in flight starts its wait once the ledger reports it landed. A qualifying capture is requested at
   once when no settle is in force (the visit's first request: the legacy rule), else once the settle has run since the
   first qualifying capture a frame boundary saw, or since the retryable failure. Past the capture deadline a capture
   not seen in time is no capture: the legacy and frame calls end the check there, as before. The 50 ms legacy schedule
   is not moved. */
static void
quiet_event_wait_capture(const dsd_opts* opts, scan_quiet_check* c, uint64_t now_ms) {
    if (!c->origin_set) {
        if (!dsd_trunk_tuning_request_landed(c->key.tune_request)) {
            return;
        }
        c->origin_set = 1;
        c->origin_ms = now_ms;
    }
    if (now_ms >= quiet_capture_deadline_ms(c, 1) && !c->capture_seen) {
        /* Too late for a capture: nothing can be requested, so the word is taken as read (each change wakes the
           service once) and the legacy and frame calls end the check. */
        c->capture_read = 1;
        c->capture_qualified = 0;
        c->capture_seq_seen = rtl_signal_capture_seq();
        return;
    }
    dsd_signal_capture cap;
    if (!quiet_landed_capture(c, &cap)) {
        return;
    }
    /* A settle in force before this sighting (a frame boundary saw the capture, or a retry's failure set it) is waited
       out; the sighting is kept either way, so the deadline is extended as a frame boundary's sighting extends it. */
    const int settle_in_force = c->settle_set;
    if (!c->capture_seen) {
        c->capture_seen = 1;
        if (!c->settle_set) {
            c->settle_set = 1;
            c->settle_from_ms = now_ms;
        }
    }
    if (settle_in_force && quiet_age_ms(now_ms, c->settle_from_ms) < SCAN_QUIET_FRAME_SETTLE_MS) {
        return;
    }
    const uint8_t refusal = quiet_capture_refusal(opts, c, &cap);
    if (refusal != (uint8_t)DSD_SCAN_QUIET_REASON_NONE) {
        quiet_refuse(c, refusal, cap.generation);
        return;
    }
    quiet_request(opts, c, &cap, now_ms);
}

/* Whether the event service has anything to do for @p c: lock-free loads, and a clock read only while a settle or a
   frame verdict's hold runs out. */
static int
quiet_event_due(const dsd_engine_scan_owner* owner, const scan_quiet_check* c) {
    switch (c->phase) {
        case SCAN_QUIET_WAIT_CAPTURE:
            if (!c->origin_set) {
                return dsd_trunk_tuning_request_landed(c->key.tune_request);
            }
            if (!c->capture_read || rtl_signal_capture_seq() != c->capture_seq_seen) {
                return 1;
            }
            /* A qualifying capture waiting out a settle: due once the settle has run, and read again then. */
            return c->capture_qualified && c->settle_set
                   && quiet_age_ms(quiet_realtime_ms(), c->settle_from_ms) >= SCAN_QUIET_FRAME_SETTLE_MS;
        case SCAN_QUIET_REQUESTED: return rtl_signal_quiet_status(c->ticket) != 0;
        case SCAN_QUIET_READY: return !c->event_handed && !quiet_pending(owner);
        default: return 0;
    }
}

int
dsd_engine_scan_quiet_event_service(const dsd_opts* opts, dsd_state* state) {
    /* The setting before anything else: with it off, the null check and one load. */
    if (!opts || !opts->scan_skip_silent) {
        return DSD_SCAN_QUIET_NONE;
    }
    dsd_engine_scan_owner* owner = dsd_engine_scan_owner_get(state);
    scan_quiet_check* c = quiet_check_get(owner);
    if (!c || !quiet_check_active(c)) {
        return DSD_SCAN_QUIET_NONE;
    }
    if (!quiet_event_due(owner, c)) {
        return DSD_SCAN_QUIET_LIVE;
    }
    const uint64_t now_ms = quiet_realtime_ms();
    /* The visit only: a READY result past its age is declined here, and ended by the legacy services as before. */
    quiet_reconcile_visit(opts, state, owner, c);
    if (c->phase == SCAN_QUIET_WAIT_CAPTURE) {
        quiet_event_wait_capture(opts, c, now_ms);
    } else if (c->phase == SCAN_QUIET_REQUESTED) {
        quiet_collect(c, now_ms, 1);
    }
    if (!quiet_check_active(c)) {
        return DSD_SCAN_QUIET_NONE;
    }
    if (c->phase == SCAN_QUIET_READY && !c->event_handed && !quiet_pending(owner)) {
        c->event_handed = 1;
        if (!quiet_ready_aged(c, now_ms) && quiet_ready_passes(opts, state, c)) {
            return DSD_SCAN_QUIET_HAND_BACK;
        }
    }
    return DSD_SCAN_QUIET_LIVE;
}

/* A visit SUSPENDED whose revisions moved since it was published: once per change, so it is republished and starts
   again on the same visit under syncs that keep the frame loop running (as a suspension of a check under way owes one,
   R5). That is a visit with a reason it cannot be checked (a refusal, or facts with no check) and a width or row edit,
   and equally one suspended with no reason to show, for its own conditions (an operator hold, a stay, no alternate, a
   tune pending): releasing a hold moves only quiet_inv_seq, and its eligibility is checked again here. */
static int
quiet_suspended_facts_moved(const dsd_opts* opts, const dsd_state* state, scan_quiet_check* c) {
    scan_quiet_revisions now_rev;
    scan_quiet_revisions_read(opts, state, &now_rev);
    if (quiet_revisions_equal(&now_rev, &c->revisions)) {
        c->republish_seen = c->revisions; /* back to what was published: a change after this owes again */
        return 0;
    }
    if (quiet_revisions_equal(&now_rev, &c->republish_seen)) {
        return 0;
    }
    c->republish_seen = now_rev;
    return 1;
}

int
dsd_engine_scan_quiet_take_republish(const dsd_opts* opts, dsd_state* state, int owner_kind) {
    if (!opts || !opts->scan_skip_silent) {
        return 0;
    }
    scan_quiet_check* c = quiet_check_get(dsd_engine_scan_owner_get(state));
    if (!c || c->phase != SCAN_QUIET_SUSPENDED || c->key.owner != owner_kind) {
        return 0;
    }
    if (c->republish_owed) {
        c->republish_owed = 0;
        /* It covers the revisions in force now: they owe no second republication for the same suspension. */
        scan_quiet_revisions_read(opts, state, &c->republish_seen);
        return 1;
    }
    if (c->refused) {
        /* The stream may run a generation the refusal never saw (the radio worker applied a supported rate or rotation
           just after it) with no revision moved: one republication, so the visit is checked again on it. */
        const uint32_t generation = dsd_rtl_stream_metrics_hook_stream_generation();
        if (generation != c->refused_generation && generation != c->refused_republish_generation) {
            c->refused_republish_generation = generation;
            return 1;
        }
    }
    return quiet_suspended_facts_moved(opts, state, c);
}

int
scan_quiet_apply(const dsd_opts* opts, dsd_state* state, int owner_kind, const scan_quiet_eligibility* current) {
    dsd_engine_scan_owner* owner = dsd_engine_scan_owner_get(state);
    scan_quiet_check* c = quiet_check_get(owner);
    if (!opts || !opts->scan_skip_silent || !c || c->phase != SCAN_QUIET_READY || c->key.owner != owner_kind) {
        return 0;
    }
    quiet_reconcile(opts, state, owner, c, quiet_realtime_ms());
    if (c->phase != SCAN_QUIET_READY || c->key.epoch == 0U || quiet_pending(owner)) {
        /* Declined by the reconcile, a park whose landing the coordinator has not noted yet, or held while a frame's
           verdict is pending (kept READY: neither applied nor declined). */
        return 0;
    }
    c->phase = SCAN_QUIET_DONE; /* consumed, or declined: one skip per visit */
    return quiet_measured_as_current(c, current) && quiet_ready_passes(opts, state, c);
}

void
scan_quiet_reconcile_declined(const dsd_opts* opts, dsd_state* state, int owner_kind, int guard_lost) {
    dsd_engine_scan_owner* owner = dsd_engine_scan_owner_get(state);
    scan_quiet_check* c = quiet_check_get(owner);
    if (!opts || !c) {
        return;
    }
    quiet_reconcile(opts, state, owner, c, quiet_realtime_ms());
    if (c->phase == SCAN_QUIET_READY && c->key.owner == owner_kind && !guard_lost && !quiet_pending(owner)) {
        c->phase = SCAN_QUIET_DONE;
    }
}

void
dsd_engine_scan_quiet_invalidate(dsd_state* state) {
    dsd_engine_scan_owner* owner = dsd_engine_scan_owner_get(state);
    if (!owner) {
        return;
    }
    owner->quiet_inv_seq++;
    scan_quiet_check* c = quiet_check_get(owner);
    if (c && quiet_check_active(c)) {
        quiet_end(c, SCAN_QUIET_SUSPENDED);
        c->republish_owed = quiet_visit_current(owner, c, 0);
    }
}

void
scan_quiet_log_skip(const dsd_state* state, const char* name, int row) {
    const dsd_engine_scan_owner* owner = quiet_owner_const(state);
    const scan_quiet_check* c = owner ? (const scan_quiet_check*)owner->quiet : NULL;
    if (!c) {
        return;
    }
    char line[192];
    (void)scan_quiet_format_skip(line, sizeof line, name, row, c->frequency_hz, c->result.power_dbfs,
                                 c->result.floor_dbfs, c->result.max_ratio_db);
    LOG_INFO("%s", line);
}

double
scan_quiet_visit_decode_m(const dsd_state* state) {
    const dsd_engine_scan_owner* owner = quiet_owner_const(state);
    const scan_quiet_check* c = owner ? (const scan_quiet_check*)owner->quiet : NULL;
    return c ? c->visit_decode_m : 0.0;
}

scan_quiet_phase
scan_quiet_phase_get(const dsd_state* state) {
    const dsd_engine_scan_owner* owner = quiet_owner_const(state);
    const scan_quiet_check* c = owner ? (const scan_quiet_check*)owner->quiet : NULL;
    return c ? c->phase : SCAN_QUIET_SUSPENDED;
}

scan_quiet_visit_key
scan_quiet_key_get(const dsd_state* state) {
    const dsd_engine_scan_owner* owner = quiet_owner_const(state);
    const scan_quiet_check* c = owner ? (const scan_quiet_check*)owner->quiet : NULL;
    if (c) {
        return c->key;
    }
    const scan_quiet_visit_key none = {0};
    return none;
}

/* The receiver's verdict the last capture read showed, while the input it was read on is still the one in force: a
   centred capture only (TUNER), which the input and its shift settings decide for every visit. A rate is no session
   fact: a list's DSP bandwidth sets the capture rate a visit runs at, so RATE is a visit's own, read on its capture. */
static uint8_t
quiet_receiver_reason_now(const dsd_opts* opts, const scan_quiet_check* c) {
    if (c->receiver_reason != (uint8_t)DSD_SCAN_QUIET_REASON_TUNER
        || c->receiver_session != rtl_stream_session_generation() || c->receiver_input != opts->audio_in_type
        || c->receiver_device != quiet_device_hash(opts)) {
        return (uint8_t)DSD_SCAN_QUIET_REASON_NONE;
    }
    return c->receiver_reason;
}

/* A capture's verdict on the receiver (TUNER, RATE), and the channel's reasons (MODES, WIDTH, NO_WIDTH). */
static int
quiet_capture_reason(uint8_t reason) {
    return reason == (uint8_t)DSD_SCAN_QUIET_REASON_TUNER || reason == (uint8_t)DSD_SCAN_QUIET_REASON_RATE;
}

static int
quiet_channel_reason(uint8_t reason) {
    return reason == (uint8_t)DSD_SCAN_QUIET_REASON_MODES || reason == (uint8_t)DSD_SCAN_QUIET_REASON_WIDTH
           || reason == (uint8_t)DSD_SCAN_QUIET_REASON_NO_WIDTH
           || reason == (uint8_t)DSD_SCAN_QUIET_REASON_UNSUPPORTED_MODE;
}

/* Log the receiver's WARN line for the capture's verdict @p reason (TUNER or RATE), once per key. */
static void
quiet_warn_capture(const dsd_opts* opts, dsd_state* state, const scan_quiet_check* c, uint8_t reason) {
    quiet_receiver_key key;
    DSD_MEMSET(&key, 0, sizeof key);
    key.reason = reason;
    if (reason == (uint8_t)DSD_SCAN_QUIET_REASON_TUNER) {
        key.detail = (uint8_t)(dsd_opts_audio_in_dev_is_airspy_spec(opts->audio_in_dev) ? SCAN_QUIET_WARN_AIRSPY
                                                                                        : SCAN_QUIET_WARN_CENTRED);
    } else {
        key.rate_hz = c->receiver_rate_hz;
    }
    key.input = opts->audio_in_type;
    key.device = quiet_device_hash(opts);
    quiet_warn_receiver(state, &key);
}

/* The remembered WARN key of row or target @p index of scan @p which (0 trunk scan, 1 -Y), grown on first use; NULL
   when it cannot be (no WARN line then, rather than one per tick). */
static quiet_channel_key*
quiet_warned_slot(scan_quiet_check* c, int which, int index) {
    if (index < 0) {
        return NULL;
    }
    const size_t need = (size_t)index + 1U;
    if (need > c->warned_cap[which]) {
        size_t cap = c->warned_cap[which] != 0U ? c->warned_cap[which] : 16U;
        while (cap < need) {
            cap *= 2U;
        }
        quiet_channel_key* grown = (quiet_channel_key*)realloc(c->warned[which], cap * sizeof *grown);
        if (!grown) {
            return NULL;
        }
        DSD_MEMSET(grown + c->warned_cap[which], 0, (cap - c->warned_cap[which]) * sizeof *grown);
        c->warned[which] = grown;
        c->warned_cap[which] = cap;
    }
    return &c->warned[which][index];
}

static int
quiet_channel_key_equal(const quiet_channel_key* a, const quiet_channel_key* b) {
    return a->list_id == b->list_id && a->list_rev == b->list_rev && a->frequency_hz == b->frequency_hz
           && a->reason == b->reason && a->classes == b->classes && a->detail == b->detail && a->kind == b->kind
           && a->width_hz == b->width_hz && a->monitor_width_hz == b->monitor_width_hz;
}

/* Log the channel's WARN line for the check's reason on @p ref, once per key per row or target. */
static void
quiet_warn_channel(const dsd_opts* opts, scan_quiet_check* c, const scan_quiet_visit_ref* ref) {
    quiet_channel_key key;
    DSD_MEMSET(&key, 0, sizeof key);
    key.list_id = ref->list_id;
    key.reason = c->reason;
    key.classes = c->classes;
    key.list_rev = ref->list_rev;
    key.frequency_hz = c->frequency_hz;
    key.detail = scan_quiet_channel_detail(opts, c->reason, c->classes);
    key.kind = c->analog_kind;
    key.width_hz = c->analog_width_hz;
    key.monitor_width_hz = opts->monitor_input_audio == 1 ? dsd_opts_analog_width_hz(opts) : -1;
    quiet_channel_key* slot = quiet_warned_slot(c, ref->owner == SCAN_QUIET_OWNER_Y ? 1 : 0, ref->index);
    if (!slot || quiet_channel_key_equal(slot, &key)) {
        return;
    }
    *slot = key;
    char line[384];
    (void)scan_quiet_format_warn(line, sizeof line, c->reason, key.detail, 0U, ref->name, ref->index, c->frequency_hz,
                                 c->analog_kind, c->analog_width_hz);
    LOG_WARN("%s", line);
}

/* Forget every channel WARN key logged (the setting turned): each is logged again. */
static void
quiet_warn_channels_forget(scan_quiet_check* c) {
    for (int which = 0; which < 2; which++) {
        if (c->warned[which]) {
            DSD_MEMSET(c->warned[which], 0, c->warned_cap[which] * sizeof c->warned[which][0]);
        }
    }
}

/* Whether the check @p c is the one of the visit on air @p ref names. */
static int
quiet_check_names(const dsd_engine_scan_owner* owner, const scan_quiet_check* c, const scan_quiet_visit_ref* ref) {
    return c->key.owner == ref->owner && c->key.index == ref->index && c->key.session == ref->session
           && quiet_visit_on_air(owner, c);
}

/* Whether the visit's check stands SUSPENDED for its status: suspended now, or ended by activity while suspended on facts
   that still stand (scan_quiet_check::suspension_kept). */
static int
quiet_suspension_stands(const dsd_opts* opts, const dsd_state* state, const scan_quiet_check* c) {
    if (c->phase == SCAN_QUIET_SUSPENDED) {
        return 1;
    }
    if (c->phase != SCAN_QUIET_DONE || !c->suspension_kept) {
        return 0;
    }
    scan_quiet_revisions now_rev;
    scan_quiet_revisions_read(opts, state, &now_rev);
    return quiet_revisions_equal(&now_rev, &c->revisions);
}

/* The receiver's reason for the visit, which outranks the channel's: its own capture's refusal, else the last capture's
   verdict, which stands for a visit that never reads one or still waits for its own. NONE while a check runs on.
   @p suspended: quiet_suspension_stands(). */
static uint8_t
quiet_visit_receiver_reason(const dsd_opts* opts, const scan_quiet_check* c, int suspended) {
    if (suspended && quiet_capture_reason(c->reason)) {
        return c->reason;
    }
    if (suspended || c->phase == SCAN_QUIET_WAIT_CAPTURE) {
        return quiet_receiver_reason_now(opts, c);
    }
    return (uint8_t)DSD_SCAN_QUIET_REASON_NONE;
}

/* The check's status on the visit @p ref names, with the session supported (scan_quiet_timing_fill()). */
static void
quiet_timing_fill_check(const dsd_opts* opts, dsd_state* state, const scan_quiet_visit_ref* ref,
                        dsd_scan_timing_publication* report) {
    const dsd_engine_scan_owner* owner = quiet_owner_const(state);
    scan_quiet_check* c = owner ? (scan_quiet_check*)owner->quiet : NULL;
    if (!c || !quiet_check_names(owner, c, ref)) {
        return; /* no check of this visit yet */
    }
    const int suspended = quiet_suspension_stands(opts, state, c);
    const uint8_t receiver = quiet_visit_receiver_reason(opts, c, suspended);
    if (receiver != (uint8_t)DSD_SCAN_QUIET_REASON_NONE) {
        quiet_warn_capture(opts, state, c, receiver);
        quiet_fill_no_check(state, ref, report, receiver);
        return;
    }
    if (suspended && quiet_channel_reason(c->reason)) {
        quiet_warn_channel(opts, c, ref);
        quiet_fill_no_check(state, ref, report, c->reason);
        return;
    }
    if (ref->shows_status && quiet_check_active(c)) {
        report->quiet_status = (uint8_t)DSD_SCAN_QUIET_STATUS_CHECKING;
    }
}

#else /* !USE_RADIO: no tap, so no check is ever made. */

int
scan_quiet_session_supported(const dsd_opts* opts) {
    (void)opts;
    return 0;
}

int
scan_quiet_wants_eligibility(const dsd_opts* opts, const dsd_state* state, int owner) {
    (void)opts;
    (void)state;
    (void)owner;
    return 0;
}

void
scan_quiet_publish_eligibility(const dsd_opts* opts, dsd_state* state, int owner_kind,
                               const scan_quiet_eligibility* e) {
    (void)opts;
    (void)state;
    (void)owner_kind;
    (void)e;
}

int
dsd_engine_scan_quiet_service(const dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    return DSD_SCAN_QUIET_NONE;
}

int
dsd_engine_scan_quiet_frame_service(const dsd_opts* opts, dsd_state* state, int owner_kind) {
    (void)opts;
    (void)state;
    (void)owner_kind;
    return DSD_SCAN_QUIET_NONE;
}

int
dsd_engine_scan_quiet_frame_yield(const dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    return 0;
}

int
dsd_engine_scan_quiet_event_service(const dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    return DSD_SCAN_QUIET_NONE;
}

int
dsd_engine_scan_quiet_take_republish(const dsd_opts* opts, dsd_state* state, int owner_kind) {
    (void)opts;
    (void)state;
    (void)owner_kind;
    return 0;
}

int
scan_quiet_apply(const dsd_opts* opts, dsd_state* state, int owner_kind, const scan_quiet_eligibility* current) {
    (void)opts;
    (void)state;
    (void)owner_kind;
    (void)current;
    return 0;
}

void
scan_quiet_reconcile_declined(const dsd_opts* opts, dsd_state* state, int owner_kind, int guard_lost) {
    (void)opts;
    (void)state;
    (void)owner_kind;
    (void)guard_lost;
}

void
dsd_engine_scan_quiet_invalidate(dsd_state* state) {
    dsd_engine_scan_quiet_note_change(state);
}

void
scan_quiet_log_skip(const dsd_state* state, const char* name, int row) {
    (void)state;
    (void)name;
    (void)row;
}

double
scan_quiet_visit_decode_m(const dsd_state* state) {
    (void)state;
    return 0.0;
}

scan_quiet_phase
scan_quiet_phase_get(const dsd_state* state) {
    (void)state;
    return SCAN_QUIET_SUSPENDED;
}

scan_quiet_visit_key
scan_quiet_key_get(const dsd_state* state) {
    (void)state;
    const scan_quiet_visit_key none = {0};
    return none;
}

#endif

#ifdef USE_RADIO
/* The setting turned on mid-visit: the visit landed now gets no check, since the checked content and carriers it heard
   while the setting was off moved no activity sequence, and it asks for no eligibility from now. A visit tuned before
   the setting turned on whose landing is not noted yet (in flight, or landed before the coordinator noted it) gets
   none either, at its publication (quiet_publication_ends_visit()): the setting applies from the next tune. */
static void
quiet_skip_landed_visit(dsd_engine_scan_owner* owner) {
    const dsd_engine_scan_landing* landing = &owner->landing;
    if (landing->by != DSD_ENGINE_SCAN_LANDING_ROW && landing->by != DSD_ENGINE_SCAN_LANDING_TARGET) {
        return;
    }
    scan_quiet_check* c = quiet_check_ensure(owner);
    if (!c) {
        return;
    }
    quiet_end(c, SCAN_QUIET_DONE);
    c->key.owner = landing->by == DSD_ENGINE_SCAN_LANDING_ROW ? SCAN_QUIET_OWNER_Y : SCAN_QUIET_OWNER_TRUNK;
    c->key.index = landing->index;
    c->key.session = landing->session;
    c->key.epoch = landing->epoch;
    c->key.tune_request = landing->tune_request;
    c->epoch_counter = owner->landing_epoch;
    c->frequency_hz = landing->hz;
}
#endif

void
scan_quiet_timing_fill(const dsd_opts* opts, dsd_state* state, const scan_quiet_visit_ref* ref,
                       dsd_scan_timing_publication* report) {
    if (!report) {
        return;
    }
    report->quiet_status = (uint8_t)DSD_SCAN_QUIET_STATUS_NONE;
    report->quiet_reason = (uint8_t)DSD_SCAN_QUIET_REASON_NONE;
    if (!opts || !state || !ref || !opts->scan_skip_silent || !ref->conventional) {
        return;
    }
#ifdef USE_RADIO
    if (scan_quiet_session_supported(opts)) {
        quiet_timing_fill_check(opts, state, ref, report);
        return;
    }
#endif
    /* The input has no check (a radio-off build: none ever): the session's reason, outranking all others. */
    quiet_receiver_key key;
    DSD_MEMSET(&key, 0, sizeof key);
    key.reason = (uint8_t)DSD_SCAN_QUIET_REASON_INPUT;
    key.detail = (uint8_t)quiet_input_detail(opts);
    key.input = opts->audio_in_type;
    key.device = quiet_device_hash(opts);
    quiet_warn_receiver(state, &key);
    quiet_fill_no_check(state, ref, report, (uint8_t)DSD_SCAN_QUIET_REASON_INPUT);
}

void
dsd_engine_scan_quiet_setting_changed(dsd_state* state, int turned_on, int reading_changed) {
    dsd_engine_scan_quiet_invalidate(state);
    /* Nothing noted while the setting was on is waited for once it changes. */
    dsd_engine_scan_quiet_pending_reset(state);
    /* Every WARN line is logged again once the setting is back on. */
    quiet_warn_receiver_forget(state);
#ifdef USE_RADIO
    dsd_engine_scan_owner* owner = dsd_engine_scan_owner_get(state);
    /* The setting turned: the visit's activity baseline is set again at its next publication (a landed visit's from
       its landing note, a park in flight's from then). Turned on, it applies from the next scan tune: no visit tuned
       before it is checked, landed or not. */
    scan_quiet_check* c = quiet_check_get(owner);
    if (c) {
        c->visit_set = 0;
        quiet_warn_channels_forget(c);
        /* A shortened reading's WARN lines are logged again once it takes effect again: after the switch or the
           length really changed, not after an apply that left both as they were. */
        if (reading_changed) {
            DSD_MEMSET(&c->short_warned, 0, sizeof c->short_warned);
            DSD_MEMSET(&c->dmr_warned, 0, sizeof c->dmr_warned);
        }
    }
    if (turned_on && owner) {
        owner->quiet_first_tune_seq = owner->tune_seq + 1U;
        quiet_skip_landed_visit(owner);
    }
#else
    (void)turned_on;
    (void)reading_changed;
#endif
}

/* Wait for a PARTIAL verdict for @p hold_ms more (capped), from now on the decode clock; a longer wait already running
   stays. */
static void
quiet_pending_extend(dsd_engine_scan_owner* owner, uint32_t hold_ms) {
    const uint32_t capped = hold_ms < SCAN_QUIET_PENDING_MAX_MS ? hold_ms : SCAN_QUIET_PENDING_MAX_MS;
    const double until_m = dsd_decode_now_mono_s() + (double)capped / 1000.0;
    if (until_m > owner->pending_until_m) {
        owner->pending_until_m = until_m;
    }
}

void
dsd_engine_scan_quiet_note_evidence(dsd_state* state, int level, uint32_t hold_ms) {
    dsd_engine_scan_owner* owner = dsd_engine_scan_owner_get(state);
    if (!owner) {
        return;
    }
    /* A transmission its sync words alone confirmed (issue #890) is no activity: the check waits on it as on a verdict
       still open, so frames that keep coming never let a quiet result be handed back sooner than activity would, and
       the check completes once they stop. */
    if (level == DSD_CONTENT_EVIDENCE_SYNC_ONLY) {
        level = DSD_CONTENT_EVIDENCE_PARTIAL;
    }
    if (level == DSD_CONTENT_EVIDENCE_PROVEN) {
        /* Activity, once a frame however many of its checks pass; it dominates: nothing is waited for any more. */
        if (!owner->frame_pending || owner->frame_evidence != (uint8_t)DSD_CONTENT_EVIDENCE_PROVEN) {
            owner->activity_seq++;
        }
        if (owner->frame_pending) {
            owner->frame_evidence = (uint8_t)DSD_CONTENT_EVIDENCE_PROVEN;
        }
        owner->frame_hold_ms = 0U;
        owner->pending_until_m = 0.0;
        return;
    }
    if (level != DSD_CONTENT_EVIDENCE_PARTIAL) {
        return;
    }
    if (!owner->frame_pending) {
        quiet_pending_extend(owner, hold_ms);
        return;
    }
    if (owner->frame_evidence == (uint8_t)DSD_CONTENT_EVIDENCE_PROVEN) {
        return; /* the frame proved its content already */
    }
    owner->frame_evidence = (uint8_t)DSD_CONTENT_EVIDENCE_PARTIAL;
    if (hold_ms > owner->frame_hold_ms) {
        owner->frame_hold_ms = hold_ms;
    }
}

void
dsd_engine_scan_quiet_note_frame_sync(dsd_state* state) {
    dsd_engine_scan_owner* owner = dsd_engine_scan_owner_get(state);
    if (owner) {
        owner->frame_pending = 1U;
        owner->frame_evidence = 0U;
        owner->frame_hold_ms = 0U;
    }
}

void
dsd_engine_scan_quiet_frame_end(dsd_state* state) {
    dsd_engine_scan_owner* owner = dsd_engine_scan_owner_get(state);
    if (!owner || !owner->frame_pending) {
        return;
    }
    /* PROVEN cleared the wait at its note. A frame that checked nothing leaves a wait another frame started running:
       a rejected sync never cancels a verdict the protocol still holds (an NXDN LICH reject keeps the weak streak). */
    if (owner->frame_evidence == (uint8_t)DSD_CONTENT_EVIDENCE_PARTIAL) {
        quiet_pending_extend(owner, owner->frame_hold_ms);
    }
    owner->frame_pending = 0U;
    owner->frame_evidence = 0U;
    owner->frame_hold_ms = 0U;
}

void
dsd_engine_scan_quiet_pending_reset(dsd_state* state) {
    dsd_engine_scan_owner* owner = dsd_engine_scan_owner_get(state);
    if (owner) {
        dsd_engine_scan_owner_pending_clear(owner);
    }
}
