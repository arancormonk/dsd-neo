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
    DSD_SCAN_MODE_M17
} dsd_scan_mode;

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
dsd_decode_mode_profile dsd_scan_mode_profile(dsd_scan_mode mode);
/** Active class, including combined P25; INHERIT when no override is installed. */
dsd_scan_mode dsd_scan_mode_active(const dsd_state* state);
/** Capture/restore effective fields for a staged tune; no pointers or audio sink fields are changed. */
void dsd_scan_settings_capture(const dsd_opts* opts, const dsd_state* state, dsd_scan_settings* out);
void dsd_scan_settings_restore(const dsd_scan_settings* saved, dsd_opts* opts, dsd_state* state);
/** Compare the acquisition-relevant settings (decoder set, modulation, inversion, slot policy, output
 * name), ignoring unused label bytes and the row-scoped option fields; optionally include live
 * timing/modulation. A difference means a staged tune or parked row must be re-acquired. */
int dsd_scan_settings_equal(const dsd_scan_settings* a, const dsd_scan_settings* b, int include_timing);
/** Prepare production row settings without committing the row or baseline. */
int dsd_scan_mode_prepare(dsd_opts* opts, dsd_state* state, dsd_scan_mode mode, dsd_scan_settings* out);
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
 * AUDIO_IN_RTL. enter pushes nothing: the options call that completes the row pushes once when
 * the level differs from the one the demod held before enter, so a row change hands the demod at
 * most one level and never the configured default in between. Callers install the row's options
 * (NULL for none) after every enter; until they do, the demod keeps the outgoing level. options
 * on its own and leave push when the level changed. resume, and an enter or leave that finds the
 * scope suspended, always push, because the command that ran while suspended may have pushed the
 * configured default itself (or the demod still holds the row's). prepare never pushes. */
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
 * (the caller hands it to the demod), 0 when a row override shadows it, -1 without opts. */
int dsd_scan_mode_set_configured_squelch(dsd_opts* opts, const dsd_state* state, double level);
/** Deep-copy scalar scope metadata for frontend snapshots. No live extension pointer is shared. */
void dsd_scan_mode_copy_snapshot(dsd_state* dst, const dsd_state* src);
/** Current class profile; combined P25 and inherited settings follow the active hunt index. */
dsd_decode_mode_profile dsd_scan_mode_effective_profile(const dsd_opts* opts, const dsd_state* state);
#ifdef __cplusplus
}
#endif
#endif
