// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/** @file @brief Scoped scanner decoder classes, independent of global CLI preset IDs. */
#ifndef DSD_NEO_RUNTIME_SCAN_MODE_H
#define DSD_NEO_RUNTIME_SCAN_MODE_H
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DSD_SCAN_MODE_INHERIT = 0,
    DSD_SCAN_MODE_P25,
    DSD_SCAN_MODE_DMR,
    DSD_SCAN_MODE_NXDN96,
    DSD_SCAN_MODE_NXDN48,
    DSD_SCAN_MODE_DPMR,
    DSD_SCAN_MODE_DSTAR,
    DSD_SCAN_MODE_YSF,
    DSD_SCAN_MODE_M17,
    /** Analog narrowband FM monitor (issue #526): the -fA receive path, one channel width per row. */
    DSD_SCAN_MODE_NFM
} dsd_scan_mode;

/** The last scan class: the bound every range check uses instead of a literal class. Appending a
 * class moves it, so stored values and MODE_BIT() option masks keep their meaning. */
#define DSD_SCAN_MODE_LAST DSD_SCAN_MODE_NFM

/** Target modulation precedence shared by scope reapplication and trunk entry. */
typedef enum {
    DSD_SCAN_MODULATION_INHERIT = 0,
    DSD_SCAN_MODULATION_AUTO,
    DSD_SCAN_MODULATION_C4FM,
    DSD_SCAN_MODULATION_CQPSK,
    DSD_SCAN_MODULATION_GFSK
} dsd_scan_modulation;

/** Scalar snapshot of the exact configured decoder settings; owns no pointers.
 * The leading block holds the row-scoped nonsecret options (see scan_options.h); they are
 * captured and restored with the rest but excluded from dsd_scan_settings_equal(). */
typedef struct {
    /** dsd_opts::rtl_squelch_level (mean power; 0 = off). First so the struct has no padding.
     * A double: compare it with a tolerance, never ==. */
    double rtl_squelch_level;
    int force_key;
    int aggressive_framesync;
    int dmr_crc_relaxed_default;
    int scan_voice_only;
    int scan_voice_qualify_ms;
    int scan_voice_hold_ms;
    int scan_max_visit_ms;
    int dmr_mute_encL;
    int dmr_mute_encR;
    int unmute_encrypted_p25;
    int trunk_tune_data_calls;
    int trunk_tune_enc_calls;
    int p25_prefer_candidates;
    char group_in_file[1024];
    int frame_dstar;
    int frame_x2tdma;
    int frame_p25p1;
    int frame_p25p2;
    int frame_nxdn48;
    int frame_nxdn96;
    int frame_dmr;
    int frame_dpmr;
    int frame_provoice;
    int frame_ysf;
    int frame_m17;
    int mod_c4fm;
    int mod_qpsk;
    int mod_gfsk;
    int mod_cli_lock;
    int mod_p25p2_c4fm;
    int mod_p25p2_profile_lock;
    int inverted_p2;
    int inverted_x2tdma;
    int inverted_dmr;
    int inverted_dpmr;
    int inverted_ysf;
    int inverted_m17;
    int dmr_stereo;
    int dmr_mono;
    int use_cosine_filter;
    int ssize;
    int msize;
    int analog_only;
    int monitor_input_audio;
    int analog_demod;
    /** dsd_opts::analog_nfm_bandwidth_hz / analog_am_bandwidth_hz (Hz, 0 = the kind's default). Acquisition
     * fields: a row width (DSD_SCAN_OPT_BANDWIDTH) lands here, and a different width restages the tune. */
    int analog_nfm_bandwidth_hz;
    int analog_am_bandwidth_hz;
    char output_name[1024];
    int state_rf_mod;
    int state_samplesPerSymbol;
    int state_symbolCenter;
    int state_dmr_stereo;
    int state_sps_hunt_idx;
} dsd_scan_settings;

/** Parse a trimmed, case-insensitive class; empty means inherit. Returns -1 on invalid input. */
int dsd_scan_mode_parse(const char* text, dsd_scan_mode* mode);
const char* dsd_scan_mode_name(dsd_scan_mode mode);
/** Nonzero for an analog class (NFM): no frames, keys, talkgroups or symbol clock, and activity is carrier. */
int dsd_scan_mode_is_analog(dsd_scan_mode mode);
/** The class to suggest for a spelling that is no class but names one (the analog FM aliases "fm", "analog",
 * "wfm", "nbfm" and "fm-conventional" suggest "nfm"), trimmed and case-insensitive; NULL for anything else. No
 * alias is ever accepted: a diagnostic offers the returned name instead. */
const char* dsd_scan_mode_alias_hint(const char* text);
/** Write every class name, "p25, dmr, ..., nfm", into @p out for a diagnostic. Returns 0, or -1 when @p out is
 * NULL or too small (it then holds an empty string). */
int dsd_scan_mode_names_list(char* out, size_t out_size);
dsd_decode_mode_profile dsd_scan_mode_profile(dsd_scan_mode mode);
/** Active class, including combined P25; INHERIT when no override is installed. */
dsd_scan_mode dsd_scan_mode_active(const dsd_state* state);
/** Capture/restore effective fields for a staged tune; no pointers or audio sink fields are changed. */
void dsd_scan_settings_capture(const dsd_opts* opts, const dsd_state* state, dsd_scan_settings* out);
void dsd_scan_settings_restore(const dsd_scan_settings* saved, dsd_opts* opts, dsd_state* state);
/** Compare the acquisition-relevant settings (decoder set, modulation, inversion, slot policy, analog
 * demodulator, output name, and for the analog family the channel widths), ignoring unused label bytes and the
 * row-scoped option fields; optionally include live timing/modulation. A difference means a staged tune or parked
 * row must be re-acquired. */
int dsd_scan_settings_equal(const dsd_scan_settings* a, const dsd_scan_settings* b, int include_timing);
/** Prepare production row settings without committing the row or baseline. @p row carries the row's
 * nonsecret options (NULL = none): the acquisition ones among them, the analog channel width, are in
 * @p out, so a scanner tunes with the settings the row will run on. */
int dsd_scan_mode_prepare(dsd_opts* opts, dsd_state* state, dsd_scan_mode mode, const dsd_scan_option_values* row,
                          dsd_scan_settings* out);
/** Reserve scope storage before staging a tune or building trunk-target snapshots. */
int dsd_scan_mode_begin(const dsd_opts* opts, dsd_state* state);
/** Configured preset for mode selectors; active combined P25 remains a separate scan class. */
dsdneoUserDecodeMode dsd_scan_mode_configured_preset(const dsd_opts* opts, const dsd_state* state);
/** Configured preset for persistence; UNSET preserves custom decoder combinations. */
dsdneoUserDecodeMode dsd_scan_mode_configured_preset_exact(const dsd_opts* opts, const dsd_state* state);
/** Select a row from the saved baseline, keeping the open audio sink layout fixed.
 * INHERIT restores the baseline for a blank row while retaining scan ownership. */
int dsd_scan_mode_enter(dsd_opts* opts, dsd_state* state, dsd_scan_mode mode);
/** Install nonsecret row overrides after mode entry; NULL restores baseline row options.
 * No allocation. Returns -1 without touching anything when no scan scope is owned (call
 * dsd_scan_mode_enter()/dsd_scan_mode_begin() first), else 0. While the scope is suspended the
 * values are only recorded and take effect at dsd_scan_mode_resume(); they are reapplied over the
 * refreshed baseline after every operator update.
 *
 * Squelch (DSD_SCAN_OPT_SQUELCH) is the one row option with hardware behind it. The effective
 * level reaches the RTL demodulator through the runtime metrics hook, and only when the input is
 * AUDIO_IN_RTL. enter never pushes: the options call that completes the row pushes once when the
 * level differs from the one the demod held before enter, so a row change hands the demod at most
 * one level and never the configured default in between. Every caller of dsd_scan_mode_enter()
 * must therefore follow it with this call (NULL for a row without options); until it does, the
 * demod keeps the outgoing level. options on its own and leave push when the level changed.
 * resume, and a leave that finds the scope suspended, always push, because the command that ran
 * while suspended may have pushed the configured default itself (or the demod still holds the
 * row's). After an enter that found the scope suspended, the options call that completes the row
 * pushes unconditionally for the same reason. prepare never pushes.
 *
 * The analog channel width (DSD_SCAN_OPT_BANDWIDTH, issue #526) is an acquisition setting, not policy:
 * options restores the configured widths and applies the row's, but nothing retunes here, so the tune
 * that lands the row must already carry it (prepare with the row's values). */
int dsd_scan_mode_options(dsd_opts* opts, dsd_state* state, const dsd_scan_option_values* values);
/** Restore the exact configured baseline and release the scope. */
void dsd_scan_mode_leave(dsd_opts* opts, dsd_state* state);
/** Temporarily restore configuration for an operator update, retaining the row constraint. */
int dsd_scan_mode_suspend(dsd_opts* opts, dsd_state* state);
/** Save updated configuration and reapply the constraint. Return nonzero when decoder
 * settings changed and acquisition must reset; audio-routing and row-scoped option updates
 * (forcing, CRC policy, mutes, voice gate, group file, data/encrypted-call policy) return zero and simply take effect. */
int dsd_scan_mode_resume(dsd_opts* opts, dsd_state* state);
/** Nonzero between suspend and resume; side effects must wait until effective settings are known. */
int dsd_scan_mode_updating(const dsd_state* state);
/** Retain target modulation precedence across configured-setting updates. */
void dsd_scan_mode_target_modulation(const dsd_state* state, dsd_scan_modulation modulation);
/** Apply target flags/locks only; AUTO starts P25 on C4FM and other trunk classes on GFSK. */
void dsd_scan_mode_apply_modulation(dsd_opts* opts, dsd_scan_mode mode, dsd_scan_modulation modulation);
/** Snapshot configured settings, even while a row override is active. */
void dsd_scan_mode_configured(const dsd_opts* opts, const dsd_state* state, dsd_scan_settings* out);
/** Borrow saved settings, or NULL without a scope/during an update. Use only on
 * the decoder thread or a consumer-owned snapshot; invalidated by scope updates. */
const dsd_scan_settings* dsd_scan_mode_configured_view(const dsd_state* state);
/** Nonsecret row-option mask, also available on a held frontend snapshot. */
uint32_t dsd_scan_mode_option_fields(const dsd_state* state);
/** Borrow the installed nonsecret row options (fields meaningful per their `present` bits), or
 * NULL without a scope. Unlike the configured view it stays valid while suspended, so a command
 * editing a configured default can tell whether the row on air shadows it. Decoder thread or a
 * consumer-owned snapshot only; invalidated by scope updates. */
const dsd_scan_option_values* dsd_scan_mode_row_options(const dsd_state* state);
/** Edit the configured squelch default (a dsd_opts::rtl_squelch_level mean power, 0 = off) without
 * suspending the scope, so no acquisition setting is compared or reset. Without a scope, or while
 * one is suspended, dsd_opts holds the configured values and takes the level. Under a live scope
 * the configured baseline takes it, and dsd_opts does too unless the installed row options
 * override the squelch. Nothing is pushed. Returns 1 when the level is now in force in dsd_opts
 * (the caller hands it to the demod), 0 when a row override shadows it, -1 without opts. It writes
 * the scan scope attached to @p state; the pointer is const only because that scope lives in the
 * state's extension slot, as with dsd_scan_mode_target_modulation(). Call it only on the decoder
 * thread with the live state, never with a frontend snapshot (dsd_app_get_latest_snapshot()): it
 * would edit the snapshot's scope copy while frontends read it, and the live scope would not change. */
int dsd_scan_mode_set_configured_squelch(dsd_opts* opts, const dsd_state* state, double level);
/** Edit the configured NFM channel width (dsd_opts::analog_nfm_bandwidth_hz, Hz, 0 = the default) without
 * suspending the scope, as the squelch setter does, so no acquisition a row has made is compared or reset (issue #526).
 * Without a scope, or while one is suspended, dsd_opts holds the configured values and takes the width. Under a live
 * scope the configured baseline takes it, and dsd_opts does too unless the installed row options set their own width
 * (DSD_SCAN_OPT_BANDWIDTH), which stays in force until the row leaves. Nothing reaches the front end here. Returns 1
 * when the width is now in force in dsd_opts (the caller hands it to the front end), 0 when a row width shadows it,
 * -1 without opts. The width is not validated. Same thread and snapshot rules as dsd_scan_mode_set_configured_squelch().
 */
int dsd_scan_mode_set_configured_nfm_bandwidth(dsd_opts* opts, const dsd_state* state, int width_hz);
/** Deep-copy scalar scope metadata for frontend snapshots. No live extension pointer is shared. */
void dsd_scan_mode_copy_snapshot(dsd_state* dst, const dsd_state* src);
/** Current class profile; combined P25 and inherited settings follow the active hunt index. */
dsd_decode_mode_profile dsd_scan_mode_effective_profile(const dsd_opts* opts, const dsd_state* state);
/** Whether the configured decode mode, not a row's class over it, is digital (issue #526): the configured view's
 * analog_only (dsd_opts' own without a live scope), with the M17 encoder, which rides the analog front end, counted as
 * digital. Only then does a digital scan row move an RTL front end still on the analog family (after an analog row)
 * onto the digital family; a typed digital row on an analog (-fA) session keeps the monitor output it has always had,
 * as svc_publish_symbol_profile() decides for a configured-mode change. */
int dsd_scan_mode_configured_digital(const dsd_opts* opts, const dsd_state* state);
/** The output rate, in Hz, a scan row's symbol timing is computed for (issue #526): the input's timing rate, or on RTL
 * input the stream's live output rate. While the RTL front end still runs the analog family after an analog row and
 * the configured mode is digital (dsd_scan_mode_configured_digital()), a row with a symbol clock (@p symbol_rate_hz >
 * 0) lands the digital family with its tune, whose output rate differs from the monitor's resampled audio rate: the
 * rate is then the one that family will run at for @p symbol_rate_hz and @p cqpsk
 * (dsd_rtl_stream_metrics_hook_output_rate_for_family()), so the decoder and the TED the tune queues are timed for the
 * samples they will get. 0 without opts. */
int dsd_scan_mode_symbol_timing_rate_hz(const dsd_opts* opts, const dsd_state* state, int symbol_rate_hz, int cqpsk);
#ifdef __cplusplus
}
#endif
#endif
