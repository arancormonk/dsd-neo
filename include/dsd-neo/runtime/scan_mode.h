// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/** @file @brief Scoped scanner decoder classes, independent of global CLI preset IDs. */
#ifndef DSD_NEO_RUNTIME_SCAN_MODE_H
#define DSD_NEO_RUNTIME_SCAN_MODE_H
#include <dsd-neo/core/analog_tone.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/power.h>
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
    DSD_SCAN_MODE_NFM,
    /** Analog AM monitor (issue #526): the -fM receive path (AM envelope detector), one channel width per row. */
    DSD_SCAN_MODE_AM
} dsd_scan_mode;

/** The last scan class: the bound every range check uses instead of a literal class. Appending a
 * class moves it, so stored values and MODE_BIT() option masks keep their meaning. */
#define DSD_SCAN_MODE_LAST DSD_SCAN_MODE_AM

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
    /** dsd_opts::analog_tone_set and analog_tone_filter, the CTCSS/DCS receive policy (issue #527): an nfm row's own
     * policy (DSD_SCAN_OPT_TONE) lands here. Row policy, so second (8-byte aligned) and out of the comparison. */
    dsd_tone_set analog_tone_set;
    int analog_tone_filter;
    /** dsd_opts::rtl_squelch_mode and rtl_squelch_margin_db (issue #518 follow-up): with rtl_squelch_level, the squelch
     * setting. Row policy, out of the comparison. */
    int rtl_squelch_mode;
    int rtl_squelch_margin_db;
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
/** Nonzero for an analog class (NFM, AM): no frames, keys, talkgroups or symbol clock, and activity is carrier. */
int dsd_scan_mode_is_analog(dsd_scan_mode mode);
/** The analog demodulator (dsd_analog_demod) an analog class runs: DSD_ANALOG_DEMOD_FM for NFM, DSD_ANALOG_DEMOD_AM for
 * AM; -1 for any other class. It names the channel width a row of the class runs, and the one its own width sets. */
int dsd_scan_mode_analog_kind(dsd_scan_mode mode);
/** The class to suggest for a spelling that is no class but names one (the analog FM aliases "fm", "analog",
 * "wfm", "nbfm" and "fm-conventional" suggest "nfm"), trimmed and case-insensitive; NULL for anything else. No
 * alias is ever accepted: a diagnostic offers the returned name instead. */
const char* dsd_scan_mode_alias_hint(const char* text);
/** Write every class name, "p25, dmr, ..., nfm, am", into @p out for a diagnostic. Returns 0, or -1 when @p out is
 * NULL or too small (it then holds an empty string). */
int dsd_scan_mode_names_list(char* out, size_t out_size);
dsd_decode_mode_profile dsd_scan_mode_profile(dsd_scan_mode mode);
/** Active class, including combined P25; INHERIT when no override is installed. */
dsd_scan_mode dsd_scan_mode_active(const dsd_state* state);
/** The class of the row the scope holds, suspended or not (INHERIT without a scope): the one dsd_scan_mode_resume()
 * reapplies over the configured settings a command is updating, which dsd_scan_mode_active() does not report while the
 * scope is suspended. Decoder thread or a consumer-owned snapshot only. */
dsd_scan_mode dsd_scan_mode_row(const dsd_state* state);
/** The settings in force when a command suspended the scope (dsd_scan_mode_suspend()): the row's constraint over the
 * configured settings, which a stream started before the command opened on or was asked for, and which
 * dsd_scan_mode_resume() puts back when the row compares unchanged. NULL unless the scope is suspended. Decoder thread
 * only. */
const dsd_scan_settings* dsd_scan_mode_suspended_effective(const dsd_state* state);
/** Capture/restore effective fields for a staged tune; no pointers or audio sink fields are changed. */
void dsd_scan_settings_capture(const dsd_opts* opts, const dsd_state* state, dsd_scan_settings* out);
void dsd_scan_settings_restore(const dsd_scan_settings* saved, dsd_opts* opts, dsd_state* state);
/** Compare the acquisition-relevant settings (decoder set, modulation, inversion, slot policy, analog
 * demodulator, output name, and for the analog family the channel width of the analog kind it runs), ignoring unused
 * label bytes, the row-scoped option fields and the width of a kind not in force; optionally include live
 * timing/modulation. A difference means a staged tune or parked row must be re-acquired. */
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
 * options restores the configured widths and applies the row's to the width of the demodulator it names
 * (dsd_scan_option_values::channel_bw_kind: an nfm row's NFM width, an am row's AM width), but nothing
 * retunes here, so the tune that lands the row must already carry it (prepare with the row's values). */
int dsd_scan_mode_options(dsd_opts* opts, dsd_state* state, const dsd_scan_option_values* values);
/** Restore the exact configured baseline and release the scope. */
void dsd_scan_mode_leave(dsd_opts* opts, dsd_state* state);
/** Temporarily restore configuration for an operator update, retaining the row constraint. */
int dsd_scan_mode_suspend(dsd_opts* opts, dsd_state* state);
/** Save updated configuration and reapply the constraint. Return nonzero when decoder
 * settings changed and acquisition must reset; audio-routing and row-scoped option updates
 * (forcing, CRC policy, mutes, voice gate, group file, data/encrypted-call policy) return zero and simply take effect.
 * A P25 row keeps its Phase 2 profile and, without a modulation lock, the modulation it acquired, and is timed once
 * both are back (dsd_scan_mode_symbol_timing_rate_hz(), issue #583). */
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
/**
 * @brief Whether a passband this client sets on a rigctl peer now is the session's, not a scan row's (issue #621).
 *
 * A scan row's reading of the peer's own passband lasts the scan, whose leave restore resets it (issue #526); the
 * session's has no such restore, so once the peer takes its own passband back the reading is retired
 * (RigctlMarkSessionPassband()). It is a scan row's while a trunk scan runs or a scan scope is held on @p state (a
 * typed -Y list's row, suspended or not); the legacy untyped -Y list holds none, has no leave restore and asks every
 * row for the session's passband, so its requests are the session's. Without @p state (a caller that cannot see the
 * scope) any configured -Y list counts as a scan. This is the scope's answer only: every rigctl leg asks
 * dsd_channel_modes_rigctl_request_is_session() (core), which passes the state wherever the caller has one and on top
 * of this answer counts a typed -Y list as a scan before its first row's scope is entered (dsd_channel_modes_present(),
 * which runtime does not see). Returns 0 for NULL @p opts. Decoder thread.
 */
int dsd_scan_mode_rigctl_request_is_session(const dsd_opts* opts, const dsd_state* state);
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
/** dsd_scan_mode_set_configured_squelch() for a whole setting (a LEVEL threshold or an AUTO margin, runtime/squelch.h):
 * the same contract. */
int dsd_scan_mode_set_configured_squelch_setting(dsd_opts* opts, const dsd_state* state,
                                                 const dsd_squelch_setting* setting);
/** Edit the configured NFM channel width (dsd_opts::analog_nfm_bandwidth_hz, Hz, 0 = the default) without
 * suspending the scope, as the squelch setter does, so no acquisition a row has made is compared or reset (issue #526).
 * Without a scope, or while one is suspended, dsd_opts holds the configured values and takes the width. Under a live
 * scope the configured baseline takes it, and dsd_opts does too unless the installed row options set their own width
 * (DSD_SCAN_OPT_BANDWIDTH), which stays in force until the row leaves. Nothing reaches the front end here. Returns 1
 * when the width is now in force in dsd_opts (the caller hands it to the front end), 0 when a row width shadows it,
 * -1 without opts. The width is not validated. Same thread and snapshot rules as dsd_scan_mode_set_configured_squelch().
 */
int dsd_scan_mode_set_configured_nfm_bandwidth(dsd_opts* opts, const dsd_state* state, int width_hz);
/** dsd_scan_mode_set_configured_nfm_bandwidth() for the configured channel width of analog demodulator @p kind
 * (dsd_analog_demod; anything but AM edits the NFM width), the AM width (dsd_opts::analog_am_bandwidth_hz, issue #524)
 * included: under a live scope the configured baseline takes it, so a row's leave or the next row's options keep the
 * edit rather than restore the width from before it. A row's own width shadows the edit only when it is a width of
 * @p kind (an nfm row's the NFM one, an am row's the AM one, issue #526): an AM edit under an nfm row, or an NFM edit
 * under an am row, reaches dsd_opts at once. Same returns and thread rules. */
int dsd_scan_mode_set_configured_analog_width(dsd_opts* opts, const dsd_state* state, int kind, int width_hz);
/** The configured channel width (Hz, 0 = the default) of analog demodulator @p kind (dsd_analog_demod; anything but
 * AM reads as NFM): what the width controls edit and a save writes, and what a row without a width of its own runs. It
 * comes from the scan scope's configured view while a scope is live, since a row's own width (issue #526) runs over
 * dsd_opts, and from dsd_opts otherwise; 0 when neither is given. Works on the live state (decoder thread) and on a
 * frontend snapshot pair alike. */
int dsd_scan_mode_configured_analog_width(const dsd_opts* opts, const dsd_state* state, int kind);
/** The configured CTCSS/DCS receive policy (issue #527): what a save writes and the frontends call the configured
 * policy. It comes from the scan scope's configured view while a scope is live, since an nfm row's own policy runs
 * over dsd_opts while the row is on air, and from dsd_opts otherwise. Writes the dsd_tone_filter_mode to @p mode and
 * the list to @p set (either may be NULL); OFF and an empty list without opts. Works on the live state (decoder
 * thread) and on a frontend snapshot pair alike. */
void dsd_scan_mode_configured_tone_policy(const dsd_opts* opts, const dsd_state* state, int* mode, dsd_tone_set* set);
/** Edit the configured CTCSS/DCS receive policy (dsd_opts::analog_tone_filter and analog_tone_set, issue #527: the live
 * tone-filter editor, DSD_APP_CMD_TONE_FILTER_SET) without suspending the scope, as the squelch and width setters do,
 * so no acquisition a row has made (a detected Phase 2 polarity, a followed call) is compared or reset. The policy is
 * not validated here. Without a scope, or while one is suspended, dsd_opts holds the configured values and takes it.
 * Under a live scope the configured baseline takes it, and dsd_opts does too unless the installed row options set their
 * own policy (DSD_SCAN_OPT_TONE, an nfm row's --tone-allow, --tone-block or --no-tone-filter), which stays in force
 * until the row leaves; the next row without one, and the leave, run the edit. Returns 1 when the policy is now in
 * force in dsd_opts, 0 when a row's own policy shadows it, -1 without opts or @p set. Same thread and snapshot rules as
 * dsd_scan_mode_set_configured_squelch(). */
int dsd_scan_mode_set_configured_tone_policy(dsd_opts* opts, const dsd_state* state, int mode, const dsd_tone_set* set);
/** Whether the configured decode mode is the analog FM monitor (-fA, the FM analog kind): the one decode mode in which
 * received-tone detection, and so the tone policy, runs (issue #527). The configured one while a scope is live, since a
 * scan row's own mode runs over dsd_opts meanwhile, else dsd_opts'. Unlike dsd_analog_tone_detection_active() it does
 * not ask whether the input carries audio. 0 for NULL opts. */
int dsd_scan_mode_configured_fm_monitor(const dsd_opts* opts, const dsd_state* state);
/** Say once that the configured CTCSS/DCS receive policy can do nothing in this session (issue #527): when it is a
 * list policy (allow or block) and @p hears_tones is 0, the caller having found nothing in the session that runs
 * received-tone detection -- neither the configured decode mode (dsd_scan_mode_configured_fm_monitor()) nor, for a
 * scan, an nfm row or nfm-conventional target, on which the row's own policy or the configured one applies while it
 * is on air (dsd_channel_modes_hear_tones() for a -Y list). Reads the configured policy
 * (dsd_scan_mode_configured_tone_policy()), never a row's. Returns 1 when it warned, 0 otherwise. */
int dsd_scan_mode_warn_tone_filter_unused(const dsd_opts* opts, const dsd_state* state, int hears_tones);
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
 * input the stream's live output rate. While the RTL front end runs the analog family after an analog row, or an
 * analog retune or request, or a retune that carries the digital family, is still outstanding (issue #583:
 * dsd_rtl_stream_metrics_hook_family_landing_after_pending()), and the configured mode is digital
 * (dsd_scan_mode_configured_digital()), a row with a symbol clock (@p symbol_rate_hz > 0) lands the digital family
 * with its tune, whose output rate differs from the monitor's resampled audio rate and from the rate a digital front
 * end runs before that landing: the rate is then the one that family will run at for @p symbol_rate_hz and @p cqpsk
 * (dsd_rtl_stream_metrics_hook_output_rate_for_family()), so the decoder and the TED the tune queues are timed for the
 * samples they will get. A row timed again while its own retune, carrying the digital family, is outstanding (a
 * resume after a command) is timed for that landing too, with the modulation the row keeps (a P25 row's learned CQPSK
 * under modulation=auto, which the resume puts back before it times the row), and the republish after it lands there
 * (svc_publish_symbol_profile() decides by this row's decision and asks for the landing). DSD_NEO_CQPSK decides that
 * CQPSK state when set, unless the scope is a --trunk-scan target that makes its own choice (issue #583,
 * dsd_scan_mode_cqpsk_explicit()): P25 with a modulation value (auto, c4fm or cqpsk), whose @p cqpsk stands, or DMR or
 * NXDN at either rate, which lands CQPSK off whatever @p cqpsk says, as the engine lands it.
 *
 * Under a scan scope this times the scope's row and records the landing decision it used
 * (dsd_scan_mode_timed_digital_family()), as the scope's own timing of a row does (dsd_scan_mode_enter(),
 * dsd_scan_mode_prepare(), dsd_scan_mode_resume()). A row once timed for the digital family's landing stays timed for
 * it, whatever the stream answers by then, until the engine spends that decision on the row's retune
 * (dsd_scan_mode_take_timed_digital_family()), which then carries the digital family whatever the stream answers: the
 * answer can fall from 1 to 0 between the two (outstanding analog work failing on another thread), and the decoder, the
 * TED and the front end are then still timed for, and land on, one family. 0 without opts. */
int dsd_scan_mode_symbol_timing_rate_hz(const dsd_opts* opts, const dsd_state* state, int symbol_rate_hz, int cqpsk);
/** Whether the scan scope's row was timed for the output rate the digital family lands on (issue #583): the landing
 * decision its last timing used (dsd_scan_mode_symbol_timing_rate_hz()), 1 for the digital family's landing, 0 for the
 * live rate. 0 without a scope, after a row's entry until the row is timed, for an analog row, off RTL input, and once
 * the engine has spent the decision (dsd_scan_mode_take_timed_digital_family()). A live republish of the row decides by
 * it without spending it (svc_publish_symbol_profile()), so a resume that timed the row for the landing is followed by
 * a republish that lands there. Decoder thread only; not carried by frontend snapshots (dsd_scan_mode_copy_snapshot()). */
int dsd_scan_mode_timed_digital_family(const dsd_state* state);
/** dsd_scan_mode_timed_digital_family(), spending the decision: the engine takes it once per retune preparation, so the
 * row's retune carries the digital family when the row was timed for it, and a later retune the row's timing did not
 * precede (a control channel hunt of a parked trunked target) decides by what the stream answers then. It writes the
 * scan scope attached to @p state; the pointer is const only because that scope lives in the state's extension slot,
 * as with dsd_scan_mode_target_modulation(). Decoder thread only, with the live state. */
int dsd_scan_mode_take_timed_digital_family(const dsd_state* state);
/** Whether the scan scope's row is a --trunk-scan target that makes its own CQPSK choice (issue #583): P25 with a
 * modulation value (auto, c4fm or cqpsk), or DMR or NXDN at either rate; and the CQPSK state the row lands with, in
 * @p out_cqpsk (may be NULL). A P25 target's own state is the modulation its decoder runs (state->rf_mod == 1, which
 * the scanner sets from the target's modulation, or an auto target's learned one); a DMR or NXDN target's is CQPSK off,
 * whatever rf_mod a -mq lock left a target with no modulation on, as the engine's GFSK chain lands it. That CQPSK state
 * stands over DSD_NEO_CQPSK where a landing on the digital family puts it: the row's timing predicts the landing with
 * it (dsd_scan_mode_symbol_timing_rate_hz(), which takes it over its caller's CQPSK state), the engine's retune for the
 * target carries it (dsd_engine_trunk_scan_cqpsk_explicit(), the same rule, and the GFSK chain's CQPSK off), and a live
 * republish of the row times the decoder, asks for the landing and publishes its symbol profile with it
 * (svc_publish_symbol_profile()). Returns 0 without opts or a scope, for a -Y row (no --trunk-scan), and for a P25
 * target with no modulation, which land where an open of the mode would: @p out_cqpsk is then the decoder's
 * (state->rf_mod == 1), which DSD_NEO_CQPSK overrides at a landing when set. */
int dsd_scan_mode_cqpsk_explicit(const dsd_opts* opts, const dsd_state* state, int* out_cqpsk);
#ifdef __cplusplus
}
#endif
#endif
